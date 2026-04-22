#include "server.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Endpoint {
    std::string host = "localhost";
    int port = 554;
};

Endpoint parse_endpoint(const std::string& value) {
    auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        throw std::runtime_error("host:port must look like 127.0.0.1:8554");
    }
    Endpoint endpoint;
    endpoint.host = value.substr(0, colon);
    endpoint.port = std::stoi(value.substr(colon + 1));
    if (endpoint.port <= 0 || endpoint.port > 65535) {
        throw std::runtime_error("port must be in 1..65535");
    }
    return endpoint;
}

void usage(const char* program) {
    std::cerr << "Usage: " << program << " [directory] [host:port]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) {
            usage(argv[0]);
            return 2;
        }
        std::filesystem::path root = argc >= 2 ? argv[1] : std::filesystem::current_path();
        Endpoint endpoint = argc >= 3 ? parse_endpoint(argv[2]) : Endpoint{};
        RtspServer server(root, endpoint.host, endpoint.port);
        return server.run();
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
