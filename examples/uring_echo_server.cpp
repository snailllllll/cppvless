/**
 * @file uring_echo_server.cpp
 * @brief io_uring Echo 服务器示例
 * 
 * 展示如何使用 IoUring 类实现高性能 Echo 服务器
 * 
 * 编译：
 *   cd build && cmake .. && make uring_echo_server
 * 
 * 运行：
 *   ./examples/uring_echo_server 12345
 * 
 * 测试：
 *   telnet 127.0.0.1 12345
 *   然后输入文字，服务器会回显
 */

#include <iostream>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>

#include "net/io_uring.h"
#include "net/socket.h"

/**
 * @brief Echo 服务器事件处理器
 * 
 * 使用 IoUring 实现 Echo 服务器
 */
class UringEchoServer {
public:
    /**
     * @brief 构造函数
     * @param port 监听端口
     * @param entries io_uring 队列大小
     */
    UringEchoServer(uint16_t port, unsigned entries = 2048);
    ~UringEchoServer();
    
    /**
     * @brief 启动服务器
     * @return true 如果启动成功
     */
    bool start();
    
    /**
     * @brief 运行事件循环
     * 
     * 阻塞直到所有连接关闭或发生错误
     */
    void run();
    
    /**
     * @brief 运行单次事件循环迭代
     * 
     * @param callback 完成事件回调函数
     * @return true 如果处理了事件
     */
    bool runOnce(const vmess::net::UringCallback& callback);
    
    /**
     * @brief 停止服务器
     */
    void stop();
    
    /**
     * @brief 获取监听端口
     */
    uint16_t port() const { return port_; }
    
    /**
     * @brief 获取当前连接数
     */
    size_t connectionCount() const { return connections_.size(); }

private:
    struct Connection {
        int fd;
        IPAddress addr;
    };
    
    uint16_t port_;
    int listenFd_;
    std::unique_ptr<vmess::net::IoUring> uring_;
    std::vector<Connection> connections_;
    bool running_;
    
    struct sockaddr_in clientAddr_;
    socklen_t clientLen_;
    
    /**
     * @brief 处理完成事件
     */
    void handleCompletion(const vmess::net::UringRequest& req, int result, uint32_t flags);
    
    /**
     * @brief 处理 accept 完成
     */
    void handleAccept(int clientFd);
    
    /**
     * @brief 处理 read 完成
     */
    void handleRead(int fd, int bytesRead, uint16_t bid);
    
    /**
     * @brief 处理 write 完成
     */
    void handleWrite(int fd, int bytesWritten, uint16_t bid);
    
    /**
     * @brief 关闭连接
     */
    void closeConnection(int fd);
};

// ============ 实现 ============

UringEchoServer::UringEchoServer(uint16_t port, unsigned entries)
    : port_(port), listenFd_(-1), running_(false) {
    
    memset(&clientAddr_, 0, sizeof(clientAddr_));
    clientLen_ = sizeof(clientAddr_);
}

UringEchoServer::~UringEchoServer() {
    stop();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

bool UringEchoServer::start() {
    // 创建监听 socket
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        perror("socket");
        return false;
    }
    
    // 设置 SO_REUSEADDR
    int opt = 1;
    if (setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    
    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (::bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    
    // 开始监听
    if (::listen(listenFd_, 128) < 0) {
        perror("listen");
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    
    // 创建 IoUring 实例
    uring_ = std::make_unique<vmess::net::IoUring>(2048);
    
    // 初始化缓冲区
    constexpr size_t BUFFER_SIZE = 2048;
    constexpr size_t BUFFER_COUNT = 4096;
    
    if (!uring_->initBuffers(1337, BUFFER_SIZE, BUFFER_COUNT)) {
        fprintf(stderr, "Failed to init buffers\n");
        return false;
    }
    
    printf("[UringEchoServer] Listening on port %d\n", port_);
    printf("[UringEchoServer] io_uring initialized with %zu buffers (%zu bytes each)\n", 
           BUFFER_COUNT, BUFFER_SIZE);
    
    running_ = true;
    
    // 准备第一个 accept 操作（不提交）
    uring_->prepareAccept(listenFd_, (struct sockaddr*)&clientAddr_, &clientLen_, 0);
    
    // 批量提交所有准备的 SQE
    uring_->submitAll();
    
    return true;
}

void UringEchoServer::stop() {
    running_ = false;
}

bool UringEchoServer::runOnce(const vmess::net::UringCallback& callback) {
    if (!uring_ || !running_) return false;
    
    // 提交并等待事件
    uring_->submitAndWait(1);
    
    // 处理完成事件
    uring_->processCompletions([this, &callback](const vmess::net::UringRequest& req, int result, uint32_t flags) {
        handleCompletion(req, result, flags);
        if (callback) {
            callback(req, result, flags);
        }
    });
    
    return true;
}

void UringEchoServer::run() {
    while (running_) {
        runOnce([](const vmess::net::UringRequest& req, int result, uint32_t flags) {
            // 默认空回调
        });
    }
}

void UringEchoServer::handleCompletion(const vmess::net::UringRequest& req, int result, uint32_t flags) {
    switch ((vmess::net::UringEventType)req.type) {
        case vmess::net::UringEventType::ACCEPT: {
            if (result >= 0) {
                handleAccept(result);
            }
            break;
        }
        case vmess::net::UringEventType::READ: {
            if (result > 0) {
                uint16_t bid = (flags >> 16);
                handleRead(req.fd, result, bid);
            } else {
                // 连接关闭或错误
                if (result == 0 || result == -ECONNRESET) {
                    closeConnection(req.fd);
                } else {
                    // 回收缓冲区
                    uring_->provideBuffer(1337, req.bid);
                }
            }
            break;
        }
        case vmess::net::UringEventType::WRITE: {
            handleWrite(req.fd, result, req.bid);
            break;
        }
        case vmess::net::UringEventType::PROV_BUF: {
            // 缓冲区回收完成
            break;
        }
        default:
            break;
    }
}

void UringEchoServer::handleAccept(int clientFd) {
    // 获取客户端地址
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    getpeername(clientFd, (struct sockaddr*)&addr, &addrLen);
    
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
    
    printf("[UringEchoServer] New connection: fd=%d, ip=%s, port=%d\n", 
           clientFd, ipStr, ntohs(addr.sin_port));
    
    // 添加到连接列表
    connections_.push_back(Connection{clientFd, IPAddress(ipStr, ntohs(addr.sin_port))});
    
    // 准备 recv 操作（不提交）
    uring_->prepareRecv(clientFd, 1337, 2048, IOSQE_BUFFER_SELECT);
    
    // 准备下一个 accept 操作（不提交）
    uring_->prepareAccept(listenFd_, (struct sockaddr*)&clientAddr_, &clientLen_, 0);
    
    // 批量提交所有准备的 SQE
    uring_->submitAll();
}

void UringEchoServer::handleRead(int fd, int bytesRead, uint16_t bid) {
    char* buffer = uring_->getBuffer(bid);
    
    printf("[UringEchoServer] Received %d bytes from fd=%d: %.*s\n", 
           bytesRead, fd, bytesRead, buffer);
    
    // Echo 回显：准备 send 操作（不提交）
    uring_->prepareSend(fd, buffer, bytesRead, 0);
    
    // 回收缓冲区（准备 provide_buffers SQE）
    uring_->provideBuffer(1337, bid);
    
    // 批量提交所有准备的 SQE
    uring_->submitAll();
}

void UringEchoServer::handleWrite(int fd, int bytesWritten, uint16_t bid) {
    if (bytesWritten > 0) {
        printf("[UringEchoServer] Sent %d bytes to fd=%d\n", bytesWritten, fd);
    }
    
    // 继续接收数据：准备 recv 操作（不提交）
    uring_->prepareRecv(fd, 1337, 2048, IOSQE_BUFFER_SELECT);
    
    // 批量提交
    uring_->submitAll();
}

void UringEchoServer::closeConnection(int fd) {
    printf("[UringEchoServer] Connection closed: fd=%d\n", fd);
    
    // 从连接列表中移除
    auto it = std::find_if(connections_.begin(), connections_.end(),
                           [fd](const Connection& c) { return c.fd == fd; });
    if (it != connections_.end()) {
        connections_.erase(it);
    }
    
    // 准备 close 操作（不提交）
    uring_->prepareClose(fd);
    
    // 批量提交
    uring_->submitAll();
}

// ============ main 函数 ============

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <port>\n", argv[0]);
        printf("Example: %s 12345\n", argv[0]);
        return 1;
    }
    
    uint16_t port = std::atoi(argv[1]);
    
    UringEchoServer server(port);
    
    if (!server.start()) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }
    
    printf("[Main] Server started on port %d\n", server.port());
    printf("[Main] Press Ctrl+C to stop\n");
    
    // 运行事件循环
    server.run();
    
    printf("[Main] Server stopped\n");
    
    return 0;
}
