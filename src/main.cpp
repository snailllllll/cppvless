#include "server/event_loop.h"
#include "common/log.h"

#include <iostream>
#include <csignal>

static vmess::server::EventLoop* g_loop = nullptr;

void signalHandler(int sig) {
    std::cerr << "\n[Main] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_loop) {
        g_loop->stop();
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 1080;
    std::string logLevel = "info";

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        logLevel = argv[2];
    }

    vmess::common::setLogLevel(vmess::common::parseLogLevel(logLevel));

    std::cerr << "=== VLESS Server ===" << std::endl;
    std::cerr << "Port: " << port << std::endl;
    std::cerr << "LogLevel: " << logLevel << std::endl;
    std::cerr << "Protocol: VLESS (plaintext, no TLS)" << std::endl;
    std::cerr << "Press Ctrl+C to stop" << std::endl;
    std::cerr << std::endl;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        vmess::server::EventLoop loop;
        g_loop = &loop;
        loop.run(port);
    } catch (const std::exception& e) {
        std::cerr << "[Main] Error: " << e.what() << std::endl;
        return 1;
    }

    std::cerr << "[Main] Server stopped" << std::endl;
    return 0;
}
