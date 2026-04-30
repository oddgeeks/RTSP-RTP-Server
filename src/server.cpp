#include "server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

using asio::ip::tcp;
using asio::ip::udp;

constexpr std::size_t kMaxInputBuffer = 256 * 1024;

struct InputCloser {
    void operator()(AVFormatContext* context) const {
        avformat_close_input(&context);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const {
        avcodec_free_context(&context);
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const {
        sws_freeContext(context);
    }
};

std::string av_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::string reason_for(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 455: return "Method Not Valid in This State";
        case 461: return "Unsupported Transport";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Error";
    }
}

rtsp::Response response_for(const rtsp::Request& request, int status) {
    rtsp::Response response;
    response.status = status;
    response.reason = reason_for(status);
    auto cseq = request.header("cseq");
    if (!cseq.empty()) {
        response.headers["CSeq"] = cseq;
    }
    response.headers["Server"] = "minimal-cpp-rtsp";
    return response;
}

std::string endpoint_address(const tcp::socket& socket) {
    std::error_code ec;
    auto endpoint = socket.remote_endpoint(ec);
    if (ec) {
        return {};
    }
    return endpoint.address().to_string();
}

}  // namespace

class RtspServer::StreamJob : public std::enable_shared_from_this<RtspServer::StreamJob> {
public:
    StreamJob(std::filesystem::path media_path,
              std::string peer_ip,
              int peer_rtp_port,
              int local_rtp_port)
        : media_path_(std::move(media_path)),
          peer_ip_(std::move(peer_ip)),
          peer_rtp_port_(peer_rtp_port),
          local_rtp_port_(local_rtp_port) {}

    ~StreamJob() {
        stop();
    }

    StreamJob(const StreamJob&) = delete;
    StreamJob& operator=(const StreamJob&) = delete;

    bool start() {
        if (thread_.joinable()) {
            return true;
        }
        try {
            thread_ = std::thread([self = shared_from_this()] {
                self->run();
            });
        } catch (const std::system_error& ex) {
            std::cerr << "failed to start stream worker: " << ex.what() << '\n';
            return false;
        }
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        try {
            const auto target = target_url();

            while (!stop_.load(std::memory_order_relaxed)) {
                stream_once(target);
            }
        } catch (const std::exception& ex) {
            std::cerr << "stream stopped: " << ex.what() << " file=" << media_path_ << '\n';
        }
    }

    std::string target_url() const {
        std::ostringstream target;
        target << "rtp://";
        if (peer_ip_.find(':') != std::string::npos) {
            target << '[' << peer_ip_ << ']';
        } else {
            target << peer_ip_;
        }
        target << ':' << peer_rtp_port_
               << "?localrtpport=" << local_rtp_port_
               << "&pkt_size=1200";
        return target.str();
    }

    void stream_once(const std::string& target) {
        AVFormatContext* input = nullptr;
        int rc = avformat_open_input(&input, media_path_.c_str(), nullptr, nullptr);
        if (rc < 0) {
            throw std::runtime_error("avformat_open_input failed: " + av_error(rc));
        }
        std::unique_ptr<AVFormatContext, InputCloser> input_guard(input);

        rc = avformat_find_stream_info(input, nullptr);
        if (rc < 0) {
            throw std::runtime_error("avformat_find_stream_info failed: " + av_error(rc));
        }

        int video_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_index < 0) {
            throw std::runtime_error("no video stream found");
        }

        AVFormatContext* muxer = nullptr;
        rc = avformat_alloc_output_context2(&muxer, nullptr, "rtp_mpegts", target.c_str());
        if (rc < 0 || muxer == nullptr) {
            throw std::runtime_error("avformat_alloc_output_context2 failed: " + av_error(rc));
        }
        std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> muxer_guard(muxer, avformat_free_context);

        AVStream* in_stream = input->streams[video_index];
        const AVCodec* decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
        if (decoder == nullptr) {
            throw std::runtime_error("no decoder found for video stream");
        }
        std::unique_ptr<AVCodecContext, CodecContextDeleter> decoder_context(avcodec_alloc_context3(decoder));
        if (!decoder_context) {
            throw std::runtime_error("avcodec_alloc_context3 decoder failed");
        }
        rc = avcodec_parameters_to_context(decoder_context.get(), in_stream->codecpar);
        if (rc < 0) {
            throw std::runtime_error("avcodec_parameters_to_context failed: " + av_error(rc));
        }
        rc = avcodec_open2(decoder_context.get(), decoder, nullptr);
        if (rc < 0) {
            throw std::runtime_error("avcodec_open2 decoder failed: " + av_error(rc));
        }

        const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
        if (encoder == nullptr) {
            encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (encoder == nullptr) {
            throw std::runtime_error("no H.264 encoder found");
        }
        std::unique_ptr<AVCodecContext, CodecContextDeleter> encoder_context(avcodec_alloc_context3(encoder));
        if (!encoder_context) {
            throw std::runtime_error("avcodec_alloc_context3 encoder failed");
        }

        AVRational frame_rate = av_guess_frame_rate(input, in_stream, nullptr);
        if (frame_rate.num <= 0 || frame_rate.den <= 0) {
            frame_rate = AVRational{30, 1};
        }
        encoder_context->codec_id = AV_CODEC_ID_H264;
        encoder_context->codec_type = AVMEDIA_TYPE_VIDEO;
        encoder_context->width = decoder_context->width;
        encoder_context->height = decoder_context->height;
        encoder_context->time_base = av_inv_q(frame_rate);
        encoder_context->framerate = frame_rate;
        encoder_context->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder_context->gop_size = 30;
        encoder_context->max_b_frames = 0;
        encoder_context->bit_rate = std::max<int64_t>(decoder_context->bit_rate, 1'500'000);
        encoder_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* encoder_options = nullptr;
        av_dict_set(&encoder_options, "preset", "ultrafast", 0);
        av_dict_set(&encoder_options, "tune", "zerolatency", 0);
        av_dict_set(&encoder_options, "x264-params", "repeat-headers=1", 0);
        rc = avcodec_open2(encoder_context.get(), encoder, &encoder_options);
        av_dict_free(&encoder_options);
        if (rc < 0) {
            throw std::runtime_error("avcodec_open2 encoder failed: " + av_error(rc));
        }

        AVStream* out_stream = avformat_new_stream(muxer, nullptr);
        if (out_stream == nullptr) {
            throw std::runtime_error("avformat_new_stream failed");
        }
        rc = avcodec_parameters_from_context(out_stream->codecpar, encoder_context.get());
        if (rc < 0) {
            throw std::runtime_error("avcodec_parameters_from_context failed: " + av_error(rc));
        }
        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = encoder_context->time_base;

        muxer->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        muxer->max_delay = 0;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "flush_packets", "1", 0);
        av_dict_set(&options, "pkt_size", "1200", 0);
        av_dict_set(&options, "localrtpport", std::to_string(local_rtp_port_).c_str(), 0);
        if (!(muxer->oformat->flags & AVFMT_NOFILE)) {
            rc = avio_open2(&muxer->pb, target.c_str(), AVIO_FLAG_WRITE, nullptr, &options);
            if (rc < 0) {
                av_dict_free(&options);
                throw std::runtime_error("avio_open2 failed: " + av_error(rc));
            }
        }
        rc = avformat_write_header(muxer, &options);
        av_dict_free(&options);
        if (rc < 0) {
            throw std::runtime_error("avformat_write_header failed: " + av_error(rc));
        }

        auto started = std::chrono::steady_clock::now();
        int64_t first_pts = AV_NOPTS_VALUE;
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("av_packet_alloc failed");
        }
        std::unique_ptr<AVPacket, PacketDeleter> packet_guard(packet);
        AVPacket* encoded = av_packet_alloc();
        if (encoded == nullptr) {
            throw std::runtime_error("av_packet_alloc encoded failed");
        }
        std::unique_ptr<AVPacket, PacketDeleter> encoded_guard(encoded);
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        std::unique_ptr<AVFrame, FrameDeleter> converted(av_frame_alloc());
        if (!frame || !converted) {
            throw std::runtime_error("av_frame_alloc failed");
        }
        converted->format = encoder_context->pix_fmt;
        converted->width = encoder_context->width;
        converted->height = encoder_context->height;
        rc = av_frame_get_buffer(converted.get(), 32);
        if (rc < 0) {
            throw std::runtime_error("av_frame_get_buffer failed: " + av_error(rc));
        }

        std::unique_ptr<SwsContext, SwsContextDeleter> scaler(
            sws_getContext(decoder_context->width,
                           decoder_context->height,
                           decoder_context->pix_fmt,
                           encoder_context->width,
                           encoder_context->height,
                           encoder_context->pix_fmt,
                           SWS_BILINEAR,
                           nullptr,
                           nullptr,
                           nullptr));
        if (!scaler) {
            throw std::runtime_error("sws_getContext failed");
        }

        while (!stop_.load(std::memory_order_relaxed) && av_read_frame(input, packet) >= 0) {
            if (packet->stream_index != video_index) {
                av_packet_unref(packet);
                continue;
            }
            rc = avcodec_send_packet(decoder_context.get(), packet);
            av_packet_unref(packet);
            if (rc < 0) {
                continue;
            }
            while (!stop_.load(std::memory_order_relaxed) &&
                   avcodec_receive_frame(decoder_context.get(), frame.get()) == 0) {
                if (first_pts == AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    first_pts = frame->best_effort_timestamp;
                }
                pace_frame(*frame, in_stream->time_base, first_pts, started);
                rc = av_frame_make_writable(converted.get());
                if (rc < 0) {
                    throw std::runtime_error("av_frame_make_writable failed: " + av_error(rc));
                }
                sws_scale(scaler.get(),
                          frame->data,
                          frame->linesize,
                          0,
                          decoder_context->height,
                          converted->data,
                          converted->linesize);
                converted->pts = next_encoder_pts_++;
                rc = avcodec_send_frame(encoder_context.get(), converted.get());
                if (rc < 0) {
                    continue;
                }
                while (avcodec_receive_packet(encoder_context.get(), encoded) == 0) {
                    av_packet_rescale_ts(encoded, encoder_context->time_base, out_stream->time_base);
                    encoded->stream_index = out_stream->index;
                    rc = av_interleaved_write_frame(muxer, encoded);
                    avio_flush(muxer->pb);
                    av_packet_unref(encoded);
                    if (rc < 0) {
                        break;
                    }
                }
            }
        }

        av_write_trailer(muxer);
        if (!(muxer->oformat->flags & AVFMT_NOFILE) && muxer->pb != nullptr) {
            avio_closep(&muxer->pb);
        }
    }

    void pace_frame(const AVFrame& frame,
                    AVRational time_base,
                    int64_t first_pts,
                    std::chrono::steady_clock::time_point started) const {
        if (frame.best_effort_timestamp == AV_NOPTS_VALUE || first_pts == AV_NOPTS_VALUE) {
            return;
        }
        const auto offset = frame.best_effort_timestamp - first_pts;
        if (offset < 0) {
            return;
        }
        const double seconds = static_cast<double>(offset) * time_base.num / time_base.den;
        const auto target = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(seconds));
        while (!stop_.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::filesystem::path media_path_;
    std::string peer_ip_;
    int peer_rtp_port_ = 0;
    int local_rtp_port_ = 0;
    int64_t next_encoder_pts_ = 0;
    std::atomic_bool stop_{false};
    std::thread thread_;
};

class RtspServer::Session : public std::enable_shared_from_this<RtspServer::Session> {
public:
    Session(tcp::socket socket, RtspServer& server)
        : socket_(std::move(socket)),
          strand_(asio::make_strand(socket_.get_executor())),
          server_(server),
          peer_ip_(endpoint_address(socket_)),
          session_id_(server_.make_session_id()) {}

    ~Session() {
        stop_stream();
    }

    void start() {
        std::cerr << "client connected " << peer_ip_ << " session=" << session_id_ << '\n';
        read_next();
    }

private:
    void read_next() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(read_buffer_),
            asio::bind_executor(strand_, [self](std::error_code ec, std::size_t n) {
                if (ec) {
                    self->close();
                    return;
                }
                self->input_.append(self->read_buffer_.data(), n);
                if (self->input_.size() > kMaxInputBuffer) {
                    self->enqueue(self->bad_request());
                    self->close();
                    return;
                }
                self->process_requests();
                self->read_next();
            }));
    }

    void process_requests() {
        while (true) {
            auto header_end = input_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                return;
            }
            auto raw = input_.substr(0, header_end + 4);
            input_.erase(0, header_end + 4);
            auto request = rtsp::parse_request(raw);
            if (!request) {
                enqueue(bad_request());
                continue;
            }
            std::cerr << peer_ip_ << " " << request->method << " " << request->uri
                      << " cseq=" << request->header("cseq") << '\n';
            enqueue(handle_request(*request).serialize());
        }
    }

    rtsp::Response handle_request(const rtsp::Request& request) {
        if (request.method == "OPTIONS") {
            auto response = response_for(request, 200);
            response.headers["Public"] = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";
            return response;
        }

        if (request.method == "DESCRIBE") {
            auto media = rtsp::resolve_media_path(server_.root_, request.uri);
            if (!media) {
                return response_for(request, 404);
            }
            uri_ = request.uri;
            media_path_ = *media;
            auto response = response_for(request, 200);
            response.headers["Content-Type"] = "application/sdp";
            response.headers["Content-Base"] = request.uri;
            response.body = rtsp::make_sdp(server_.host_, request.uri);
            return response;
        }

        if (request.method == "SETUP") {
            auto media = rtsp::resolve_media_path(server_.root_, request.uri);
            if (!media) {
                return response_for(request, 404);
            }
            auto ports = rtsp::parse_client_ports(request.header("transport"));
            if (!ports) {
                return response_for(request, 461);
            }
            auto server_ports = server_.allocate_server_ports();
            stop_stream();
            uri_ = request.uri;
            media_path_ = *media;
            client_rtp_port_ = ports->first;
            client_rtcp_port_ = ports->second;
            server_rtp_port_ = server_ports.first;
            server_rtcp_port_ = server_ports.second;
            setup_complete_ = true;

            auto response = response_for(request, 200);
            response.headers["Session"] = session_id_;
            response.headers["Transport"] = "RTP/AVP;unicast;client_port=" +
                std::to_string(client_rtp_port_) + "-" + std::to_string(client_rtcp_port_) +
                ";server_port=" + std::to_string(server_rtp_port_) + "-" +
                std::to_string(server_rtcp_port_) + ";ssrc=1234ABCD";
            return response;
        }

        if (request.method == "PLAY") {
            if (!setup_complete_) {
                return response_for(request, 455);
            }
            if (!start_stream()) {
                return response_for(request, 500);
            }
            auto response = response_for(request, 200);
            response.headers["Session"] = session_id_;
            response.headers["Range"] = "npt=0.000-";
            response.headers["RTP-Info"] = "url=" + request.uri + ";seq=0;rtptime=0";
            return response;
        }

        if (request.method == "TEARDOWN") {
            stop_stream();
            setup_complete_ = false;
            auto response = response_for(request, 200);
            response.headers["Session"] = session_id_;
            return response;
        }

        return response_for(request, 501);
    }

    bool start_stream() {
        if (stream_) {
            return true;
        }
        stream_ = std::make_shared<StreamJob>(media_path_,
                                              peer_ip_,
                                              client_rtp_port_,
                                              server_rtp_port_);
        if (!stream_->start()) {
            stream_.reset();
            return false;
        }
        std::cerr << "started in-process stream session=" << session_id_
                  << " target=" << peer_ip_ << ':' << client_rtp_port_
                  << " file=" << media_path_ << '\n';
        return true;
    }

    void stop_stream() {
        if (stream_) {
            stream_->stop();
            stream_.reset();
        }
    }

    std::string bad_request() const {
        rtsp::Response response;
        response.status = 400;
        response.reason = reason_for(400);
        response.headers["Server"] = "minimal-cpp-rtsp";
        return response.serialize();
    }

    void enqueue(std::string response) {
        const bool writing = !output_.empty();
        output_.push_back(std::move(response));
        if (!writing) {
            write_next();
        }
    }

    void write_next() {
        if (output_.empty()) {
            return;
        }
        auto self = shared_from_this();
        asio::async_write(
            socket_,
            asio::buffer(output_.front()),
            asio::bind_executor(strand_, [self](std::error_code ec, std::size_t) {
                if (ec) {
                    self->close();
                    return;
                }
                self->output_.pop_front();
                self->write_next();
            }));
    }

    void close() {
        stop_stream();
        std::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    tcp::socket socket_;
    asio::strand<tcp::socket::executor_type> strand_;
    RtspServer& server_;
    std::array<char, 8192> read_buffer_{};
    std::string input_;
    std::deque<std::string> output_;
    std::string peer_ip_;
    std::string session_id_;
    std::string uri_;
    std::filesystem::path media_path_;
    int client_rtp_port_ = 0;
    int client_rtcp_port_ = 0;
    int server_rtp_port_ = 0;
    int server_rtcp_port_ = 0;
    bool setup_complete_ = false;
    std::shared_ptr<StreamJob> stream_;
};

RtspServer::RtspServer(std::filesystem::path root,
                       std::string host,
                       int port,
                       std::size_t thread_count)
    : root_(std::move(root)),
      host_(std::move(host)),
      port_(port),
      thread_count_(std::max<std::size_t>(1, thread_count)),
      io_(static_cast<int>(thread_count_)),
      acceptor_(io_) {}

RtspServer::~RtspServer() {
    stop();
}

int RtspServer::run() {
    asio::ip::tcp::resolver resolver(io_);
    auto endpoints = resolver.resolve(host_, std::to_string(port_));
    std::error_code last_error;
    for (const auto& endpoint : endpoints) {
        acceptor_.open(endpoint.endpoint().protocol(), last_error);
        if (last_error) {
            continue;
        }
        acceptor_.set_option(tcp::acceptor::reuse_address(true), last_error);
        acceptor_.bind(endpoint.endpoint(), last_error);
        if (!last_error) {
            acceptor_.listen(asio::socket_base::max_listen_connections, last_error);
        }
        if (!last_error) {
            break;
        }
        acceptor_.close();
    }
    if (last_error) {
        throw std::runtime_error("failed to bind listener: " + last_error.message());
    }

    std::cerr << "listening on " << host_ << ':' << port_ << " root=" << root_
              << " io_threads=" << thread_count_ << '\n';
    start_accept();

    workers_.reserve(thread_count_);
    for (std::size_t i = 0; i < thread_count_; ++i) {
        workers_.emplace_back([this] {
            io_.run();
        });
    }
    for (auto& worker : workers_) {
        worker.join();
    }
    return 0;
}

void RtspServer::stop() {
    std::error_code ignored;
    acceptor_.close(ignored);
    io_.stop();
    for (auto& worker : workers_) {
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }
}

void RtspServer::start_accept() {
    acceptor_.async_accept(asio::make_strand(io_), [this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<Session>(std::move(socket), *this)->start();
        } else if (acceptor_.is_open()) {
            std::cerr << "accept failed: " << ec.message() << '\n';
        }
        if (acceptor_.is_open()) {
            start_accept();
        }
    });
}

std::string RtspServer::make_session_id() {
    std::ostringstream out;
    out << std::hex << next_session_.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}

std::pair<int, int> RtspServer::allocate_server_ports() {
    for (int attempt = 0; attempt < 5000; ++attempt) {
        int rtp = next_udp_port_.fetch_add(2, std::memory_order_relaxed);
        if (rtp > 20000) {
            next_udp_port_.store(10000, std::memory_order_relaxed);
            rtp = next_udp_port_.fetch_add(2, std::memory_order_relaxed);
        }

        try {
            asio::io_context io;
            udp::socket rtp_socket(io, udp::endpoint(udp::v4(), static_cast<unsigned short>(rtp)));
            udp::socket rtcp_socket(io, udp::endpoint(udp::v4(), static_cast<unsigned short>(rtp + 1)));
            return {rtp, rtp + 1};
        } catch (const std::system_error&) {
        }
    }
    throw std::runtime_error("no UDP RTP port pair available");
}
