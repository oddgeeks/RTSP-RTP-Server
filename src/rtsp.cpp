#include "rtsp.hpp"

#include <charconv>
#include <iomanip>

namespace rtsp {
namespace {

bool starts_with_path(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto child_it = child.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *child_it != *root_it) {
            return false;
        }
    }
    return true;
}

std::string path_from_uri(const std::string& uri) {
    auto path_start = uri.find("://");
    std::string path;
    if (path_start == std::string::npos) {
        path = uri;
    } else {
        auto slash = uri.find('/', path_start + 3);
        path = slash == std::string::npos ? "/" : uri.substr(slash);
    }
    auto query = path.find('?');
    if (query != std::string::npos) {
        path.resize(query);
    }
    return path;
}

}  // namespace

std::optional<Request> parse_request(const std::string& raw) {
    std::istringstream in(raw);
    std::string line;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    Request request;
    std::istringstream first(line);
    if (!(first >> request.method >> request.uri >> request.version)) {
        return std::nullopt;
    }

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers[lower(trim(std::string_view(line).substr(0, colon)))] =
            trim(std::string_view(line).substr(colon + 1));
    }

    std::ostringstream body;
    body << in.rdbuf();
    request.body = body.str();
    return request;
}

std::string percent_decode(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int value = 0;
            auto first = input.data() + static_cast<std::ptrdiff_t>(i + 1);
            auto result = std::from_chars(first, first + 2, value, 16);
            if (result.ec == std::errc{} && result.ptr == first + 2) {
                output.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        output.push_back(input[i]);
    }
    return output;
}

std::optional<std::filesystem::path> resolve_media_path(const std::filesystem::path& root,
                                                        const std::string& uri) {
    auto raw_path = path_from_uri(uri);
    if (raw_path.empty() || raw_path == "/") {
        return std::nullopt;
    }
    while (raw_path.starts_with('/')) {
        raw_path.erase(raw_path.begin());
    }

    std::error_code ec;
    auto canonical_root = std::filesystem::canonical(root, ec);
    if (ec) {
        return std::nullopt;
    }
    auto candidate = std::filesystem::weakly_canonical(canonical_root / percent_decode(raw_path), ec);
    if (ec || !starts_with_path(candidate, canonical_root)) {
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
        return std::nullopt;
    }
    return candidate;
}

std::string make_sdp(const std::string& server_host, const std::string& uri) {
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 0 0 IN IP4 " << server_host << "\r\n"
        << "s=Minimal RTSP Server\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n"
        << "m=video 0 RTP/AVP 96\r\n"
        << "a=rtpmap:96 H264/90000\r\n"
        << "a=fmtp:96 packetization-mode=1\r\n"
        << "a=control:" << uri << "/trackID=0\r\n";
    return sdp.str();
}

std::optional<std::pair<int, int>> parse_client_ports(const std::string& transport) {
    auto lowered = lower(transport);
    auto pos = lowered.find("client_port=");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += std::string("client_port=").size();
    auto end = lowered.find_first_of(";\r\n", pos);
    auto value = lowered.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    auto dash = value.find('-');
    if (dash == std::string::npos) {
        return std::nullopt;
    }
    int rtp = 0;
    int rtcp = 0;
    auto r1 = std::from_chars(value.data(), value.data() + dash, rtp);
    auto r2 = std::from_chars(value.data() + dash + 1, value.data() + value.size(), rtcp);
    if (r1.ec != std::errc{} || r2.ec != std::errc{} || rtp <= 0 || rtcp <= 0) {
        return std::nullopt;
    }
    return std::make_pair(rtp, rtcp);
}

}  // namespace rtsp
