#pragma once

#include "rtsp.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class RtspServer {
public:
    RtspServer(std::filesystem::path root, std::string host, int port);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    int run();

private:
    struct Client {
        int fd = -1;
        std::string peer_ip;
        std::string input;
        std::string output;
        std::string session_id;
        std::string uri;
        std::filesystem::path media_path;
        int client_rtp_port = 0;
        int client_rtcp_port = 0;
        int server_rtp_port = 0;
        int server_rtcp_port = 0;
        bool setup_complete = false;
    };

    void open_listener();
    void accept_client();
    void close_client(size_t index);
    bool read_from(Client& client);
    bool write_to(Client& client);
    void process_requests(Client& client);
    rtsp::Response handle_request(Client& client, const rtsp::Request& request);
    std::pair<int, int> allocate_server_ports() const;

    std::filesystem::path root_;
    std::string host_;
    int port_ = 554;
    int listen_fd_ = -1;
    std::vector<Client> clients_;
};
