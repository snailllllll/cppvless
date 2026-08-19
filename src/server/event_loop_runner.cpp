#include "server/event_loop_runner.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace vmess {
namespace server {

namespace {

// 当前活动 loop 集合（信号处理需要遍历停止）
const std::vector<EventLoop*>* g_loops = nullptr;

void signalHandler(int sig) {
    std::cerr << "\n[Main] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_loops) {
        for (auto* loop : *g_loops) {
            if (loop) {
                loop->stop();
            }
        }
    }
}

} // namespace

void installSignalHandler(const std::vector<EventLoop*>& loops) {
    g_loops = &loops;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}

int runEventLoops(std::vector<std::unique_ptr<EventLoop>>& loops,
                  const std::vector<uint16_t>& ports,
                  bool reusePort,
                  std::string* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    if (loops.size() != ports.size()) {
        if (errorOut) {
            *errorOut = "internal: loops/ports size mismatch";
        }
        return 1;
    }

    std::vector<EventLoop*> loopPtrs;
    loopPtrs.reserve(loops.size());
    for (auto& loop : loops) {
        loopPtrs.push_back(loop.get());
    }
    installSignalHandler(loopPtrs);

    std::atomic<bool> hasError{false};
    std::string errorMessage;
    std::mutex errorMu;
    std::vector<std::thread> workers;
    workers.reserve(loops.size());

    for (size_t i = 0; i < loops.size(); ++i) {
        workers.emplace_back([&, i]() {
            try {
                loops[i]->run(ports[i], reusePort);
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(errorMu);
                    if (!hasError.load()) {
                        errorMessage = e.what();
                    }
                }
                hasError = true;
                for (auto* loop : loopPtrs) {
                    if (loop) {
                        loop->stop();
                    }
                }
            }
        });
    }

    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (hasError) {
        if (errorOut) {
            *errorOut = errorMessage.empty() ? "worker thread failed" : errorMessage;
        }
        return 1;
    }
    return 0;
}

} // namespace server
} // namespace vmess
