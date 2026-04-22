#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <random>
#include <stdexcept>

namespace {

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("failed to set nonblocking socket");
    }
}

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

std::string make_session_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << rng();
    return out.str();
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

}  // namespace

RtspServer::RtspServer(std::filesystem::path root, std::string host, int port)
    : root_(std::move(root)), host_(std::move(host)), port_(port) {}

RtspServer::~RtspServer() {
    for (auto& client : clients_) {
        stop_stream(client);
        close_fd(client.fd);
    }
    close_fd(listen_fd_);
}

int RtspServer::run() {
    open_listener();
    std::cerr << "listening on " << host_ << ':' << port_ << " root=" << root_ << '\n';

    while (true) {
        std::vector<pollfd> fds;
        fds.push_back({listen_fd_, POLLIN, 0});
        for (const auto& client : clients_) {
            short events = POLLIN;
            if (!client.output.empty()) {
                events |= POLLOUT;
            }
            fds.push_back({client.fd, events, 0});
        }

        int ready = poll(fds.data(), fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
        }

        if (fds[0].revents & POLLIN) {
            accept_client();
        }

        for (size_t i = clients_.size(); i > 0; --i) {
            auto& poll_entry = fds[i];
            auto index = i - 1;
            bool keep = true;
            if (poll_entry.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                keep = false;
            }
            if (keep && (poll_entry.revents & POLLIN)) {
                keep = read_from(clients_[index]);
            }
            if (keep && (poll_entry.revents & POLLOUT)) {
                keep = write_to(clients_[index]);
            }
            if (!keep) {
                close_client(index);
            }
        }
    }
}

void RtspServer::open_listener() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    auto port = std::to_string(port_);
    int rc = getaddrinfo(host_.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        listen_fd_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd_ < 0) {
            continue;
        }
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(listen_fd_, ai->ai_addr, ai->ai_addrlen) == 0 && listen(listen_fd_, SOMAXCONN) == 0) {
            set_nonblocking(listen_fd_);
            freeaddrinfo(result);
            return;
        }
        close_fd(listen_fd_);
    }
    freeaddrinfo(result);
    throw std::runtime_error("failed to bind listener");
}

void RtspServer::accept_client() {
    while (true) {
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
        }
        set_nonblocking(fd);

        char host[NI_MAXHOST] = {};
        getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
        Client client;
        client.fd = fd;
        client.peer_ip = host;
        client.session_id = make_session_id();
        std::cerr << "client connected " << client.peer_ip << " session=" << client.session_id << '\n';
        clients_.push_back(std::move(client));
    }
}

void RtspServer::close_client(size_t index) {
    std::cerr << "client disconnected session=" << clients_[index].session_id << '\n';
    stop_stream(clients_[index]);
    close_fd(clients_[index].fd);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool RtspServer::read_from(Client& client) {
    char buffer[8192];
    while (true) {
        ssize_t n = recv(client.fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            client.input.append(buffer, static_cast<size_t>(n));
            process_requests(client);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
}

bool RtspServer::write_to(Client& client) {
    while (!client.output.empty()) {
        ssize_t n = send(client.fd, client.output.data(), client.output.size(), MSG_NOSIGNAL);
        if (n > 0) {
            client.output.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
    return true;
}

void RtspServer::process_requests(Client& client) {
    while (true) {
        auto header_end = client.input.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return;
        }
        auto raw = client.input.substr(0, header_end + 4);
        client.input.erase(0, header_end + 4);
        auto request = rtsp::parse_request(raw);
        if (!request) {
            rtsp::Response response;
            response.status = 400;
            response.reason = reason_for(400);
            client.output += response.serialize();
            continue;
        }
        std::cerr << client.peer_ip << " " << request->method << " " << request->uri
                  << " cseq=" << request->header("cseq") << '\n';
        client.output += handle_request(client, *request).serialize();
    }
}

rtsp::Response RtspServer::handle_request(Client& client, const rtsp::Request& request) {
    if (request.method == "OPTIONS") {
        auto response = response_for(request, 200);
        response.headers["Public"] = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";
        return response;
    }

    if (request.method == "DESCRIBE") {
        auto media = rtsp::resolve_media_path(root_, request.uri);
        if (!media) {
            return response_for(request, 404);
        }
        client.uri = request.uri;
        client.media_path = *media;
        auto response = response_for(request, 200);
        response.headers["Content-Type"] = "application/sdp";
        response.headers["Content-Base"] = request.uri;
        response.body = rtsp::make_sdp(host_, request.uri);
        return response;
    }

    if (request.method == "SETUP") {
        auto media = rtsp::resolve_media_path(root_, request.uri);
        if (!media) {
            return response_for(request, 404);
        }
        auto ports = rtsp::parse_client_ports(request.header("transport"));
        if (!ports) {
            return response_for(request, 461);
        }
        auto server_ports = allocate_server_ports();
        client.uri = request.uri;
        client.media_path = *media;
        client.client_rtp_port = ports->first;
        client.client_rtcp_port = ports->second;
        client.server_rtp_port = server_ports.first;
        client.server_rtcp_port = server_ports.second;
        client.setup_complete = true;

        auto response = response_for(request, 200);
        response.headers["Session"] = client.session_id;
        response.headers["Transport"] = "RTP/AVP;unicast;client_port=" +
            std::to_string(client.client_rtp_port) + "-" + std::to_string(client.client_rtcp_port) +
            ";server_port=" + std::to_string(client.server_rtp_port) + "-" +
            std::to_string(client.server_rtcp_port) + ";ssrc=1234ABCD";
        return response;
    }

    if (request.method == "PLAY") {
        if (!client.setup_complete) {
            return response_for(request, 455);
        }
        if (!start_stream(client)) {
            return response_for(request, 500);
        }
        auto response = response_for(request, 200);
        response.headers["Session"] = client.session_id;
        response.headers["Range"] = "npt=0.000-";
        response.headers["RTP-Info"] = "url=" + request.uri + ";seq=0;rtptime=0";
        return response;
    }

    if (request.method == "TEARDOWN") {
        stop_stream(client);
        client.setup_complete = false;
        auto response = response_for(request, 200);
        response.headers["Session"] = client.session_id;
        return response;
    }

    return response_for(request, 501);
}

bool RtspServer::start_stream(Client& client) {
    if (client.stream_pid > 0) {
        int status = 0;
        auto rc = waitpid(client.stream_pid, &status, WNOHANG);
        if (rc == 0) {
            return true;
        }
        client.stream_pid = -1;
    }

    std::ostringstream target;
    target << "rtp://";
    if (client.peer_ip.find(':') != std::string::npos) {
        target << '[' << client.peer_ip << ']';
    } else {
        target << client.peer_ip;
    }
    target << ':' << client.client_rtp_port
           << "?localrtpport=" << client.server_rtp_port
           << "&localrtcpport=" << client.server_rtcp_port
           << "&pkt_size=1200";

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "failed to fork ffmpeg: " << std::strerror(errno) << '\n';
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        const std::string input = client.media_path.string();
        const std::string url = target.str();
        execlp("ffmpeg", "ffmpeg",
               "-nostdin",
               "-hide_banner",
               "-loglevel", "error",
               "-re",
               "-stream_loop", "-1",
               "-i", input.c_str(),
               "-an",
               "-c:v", "libx264",
               "-preset", "ultrafast",
               "-tune", "zerolatency",
               "-g", "30",
               "-x264-params", "repeat-headers=1",
               "-pix_fmt", "yuv420p",
               "-f", "rtp_mpegts",
               url.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }

    client.stream_pid = pid;
    std::cerr << "started ffmpeg pid=" << pid << " session=" << client.session_id
              << " target=" << target.str() << " file=" << client.media_path << '\n';
    return true;
}

void RtspServer::stop_stream(Client& client) {
    if (client.stream_pid <= 0) {
        return;
    }
    int status = 0;
    if (waitpid(client.stream_pid, &status, WNOHANG) == 0) {
        kill(client.stream_pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (waitpid(client.stream_pid, &status, WNOHANG) != 0) {
                client.stream_pid = -1;
                return;
            }
            usleep(50000);
        }
        kill(client.stream_pid, SIGKILL);
        waitpid(client.stream_pid, &status, 0);
    }
    client.stream_pid = -1;
}

std::pair<int, int> RtspServer::allocate_server_ports() const {
    static int next = 10000;
    for (int attempt = 0; attempt < 5000; ++attempt) {
        int rtp = next;
        next += 2;
        if (next > 20000) {
            next = 10000;
        }

        int rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        int rtcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp_fd < 0 || rtcp_fd < 0) {
            close_fd(rtp_fd);
            close_fd(rtcp_fd);
            continue;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(rtp));
        bool ok = bind(rtp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        addr.sin_port = htons(static_cast<uint16_t>(rtp + 1));
        ok = ok && bind(rtcp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        close_fd(rtp_fd);
        close_fd(rtcp_fd);
        if (ok) {
            return {rtp, rtp + 1};
        }
    }
    throw std::runtime_error("no UDP RTP port pair available");
}
