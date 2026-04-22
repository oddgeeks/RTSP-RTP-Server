#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rtsp {

struct Request {
    std::string method;
    std::string uri;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string header(std::string key) const {
        std::ranges::transform(key, key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!headers.contains(key)) {
            return {};
        }
        return headers.at(key);
    }
};

struct Response {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    std::string serialize() const {
        std::ostringstream out;
        out << "RTSP/1.0 " << status << ' ' << reason << "\r\n";
        for (const auto& [key, value] : headers) {
            out << key << ": " << value << "\r\n";
        }
        out << "Content-Length: " << body.size() << "\r\n\r\n";
        out << body;
        return out.str();
    }
};

inline std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

inline std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<Request> parse_request(const std::string& raw);
std::string percent_decode(std::string_view input);
std::optional<std::filesystem::path> resolve_media_path(const std::filesystem::path& root,
                                                        const std::string& uri);
std::string make_sdp(const std::string& server_host, const std::string& uri);
std::optional<std::pair<int, int>> parse_client_ports(const std::string& transport);

}  // namespace rtsp
