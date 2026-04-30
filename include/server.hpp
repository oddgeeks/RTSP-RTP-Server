#pragma once

#include "rtsp.hpp"

#include <utility>
#include <asio.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class RtspServer {
public:
    RtspServer(std::filesystem::path root,
               std::string host,
               int port,
               std::size_t thread_count = std::thread::hardware_concurrency());
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    int run();
    void stop();

private:
    class Session;
    class StreamJob;

    void start_accept();
    std::string make_session_id();
    std::pair<int, int> allocate_server_ports();

    std::filesystem::path root_;
    std::string host_;
    int port_ = 554;
    std::size_t thread_count_ = 1;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::thread> workers_;
    std::atomic_uint64_t next_session_{1};
    std::atomic_int next_udp_port_{10000};
};
