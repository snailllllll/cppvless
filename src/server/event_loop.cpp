#include "server/event_loop.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

namespace vless {
namespace server {

EventLoop::EventLoop(ConnectionFactory factory, unsigned int entries)
    : uring_(entries), factory_(std::move(factory)) {}

void EventLoop::run(uint16_t listenPort, bool enableReusePort) {
    // 创建监听 socket
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throw std::runtime_error("failed to create listen socket");
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (enableReusePort) {
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listenPort);
    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listenFd_);
        throw std::runtime_error("failed to bind listen socket");
    }

    // backlog 4096：与 v2ray/Xray 对齐。128 在高并发（400+ 连接）下 accept 队列
    // 溢出，导致客户端连接排队 ~1.4s 的尾部延迟（实测 cpp p90 骤升而 go 正常）。
    if (::listen(listenFd_, 4096) < 0) {
        close(listenFd_);
        throw std::runtime_error("failed to listen");
    }

    int flags = fcntl(listenFd_, F_GETFL, 0);
    fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

    running_ = true;
    LOG_INFO("EventLoop", "Server listening on port ", listenPort);

    // 启动 Accept 协程
    acceptTask_ = acceptLoop();
    if (!acceptTask_.done()) {
        acceptTask_.h.resume();
    }

    // 主事件循环
    while (running_) {
        uring_.submitAndWait(1);

        // 处理完成事件：user_data 为操作对象指针，零查表分发
        uring_.processCompletions([](int result, uint32_t flags,
                                      uint64_t userData) {
            LOG_DEBUG("EventLoop", "CQE result=", result);
            coro::UringOp::completeFromCqe(userData, result, flags);
        });

        cleanupClosedConnections();
    }

    connections_.clear();
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
}

void EventLoop::stop() {
    running_ = false;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void EventLoop::acceptFd(int fd) {
    // 通过工厂创建协议连接并启动
    auto conn = factory_(fd, uring_);
    if (!conn) {
        LOG_ERROR("EventLoop", "factory returned null, closing fd=", fd);
        ::close(fd);
        return;
    }
    conn->start();
    connections_[fd] = std::move(conn);
}

coro::Task<void> EventLoop::acceptLoop() {
    LOG_INFO("EventLoop", "Accept loop started");

    while (running_) {
        int clientFd = co_await coro::AsyncAccept(listenFd_, uring_);

        if (clientFd < 0) {
            if (!running_) {
                break;
            }
            LOG_ERROR("EventLoop", "Accept failed: ", clientFd);
            continue;
        }

        LOG_INFO("EventLoop", "New connection: fd=", clientFd);

        // 设置非阻塞（io_uring accept 返回的 fd 不带 SOCK_NONBLOCK，需手动设）
        int fl = fcntl(clientFd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(clientFd, F_SETFL, fl | O_NONBLOCK);
        }
        acceptFd(clientFd);

        // 排空 backlog：oneshot accept 仅作唤醒信号，唤醒后用非阻塞 accept4
        // 把排队连接一次性抽干（突发建连从"K 轮循环消费 K 个"变为 1 轮全量就绪）。
        //
        // 前提：run() 已将 listenFd_ 设为 O_NONBLOCK——若该前提被破坏，
        // 本循环会阻塞整个 worker 线程。这是全异步 IO 路径上唯一的同步
        // 非阻塞豁免点，勿扩散（设计论证：doc/dev/design/
        // event-loop-optimization-evaluation.md §3.5）。
        while (running_) {
            int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;                                    // 排空完成
                }
                if (errno == EINTR || errno == ECONNABORTED) {
                    continue;                                 // 队列头已消耗，继续排
                }
                // EMFILE/ENFILE 等：必须退出——不消耗队列，continue 会死循环烧 CPU
                LOG_ERROR("EventLoop", "accept4 failed: ", -errno);
                break;
            }
            LOG_INFO("EventLoop", "New connection: fd=", fd);
            acceptFd(fd);
        }
    }

    LOG_INFO("EventLoop", "Accept loop ended");
}

void EventLoop::cleanupClosedConnections() {
    for (auto it = connections_.begin(); it != connections_.end(); ) {
        if (it->second->isClosed()) {
            LOG_INFO("EventLoop", "Connection closed: fd=", it->first);
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace server
} // namespace vless
