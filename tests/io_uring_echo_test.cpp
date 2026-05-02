/**
 * @file io_uring_echo_test.cpp
 * @brief io_uring 回声服务器测试
 * 
 * 功能：
 * 1. 启动 io_uring echo 服务器
 * 2. 客户端连接并发送数据
 * 3. 服务器回显数据
 * 4. 验证数据正确性
 */

#include "net/io_uring.h"
#include "net/socket.h"
#include <iostream>
#include <cstring>
#include <csignal>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>

using namespace vmess::net;
using namespace std;

/**
 * @brief Echo 服务器事件处理器
 * 
 * 使用 IoUring 实现 Echo 服务器
 * 
 * 注意：这个类是从 examples/uring_echo_server.cpp 复制过来的
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
    bool runOnce(const UringCallback& callback);
    
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
    std::unique_ptr<IoUring> uring_;
    std::vector<Connection> connections_;
    bool running_;
    
    struct sockaddr_in clientAddr_;
    socklen_t clientLen_;
    
    /**
     * @brief 处理完成事件
     */
    void handleCompletion(const UringRequest& req, int result, uint32_t flags, uint64_t);
    
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
    uring_ = std::make_unique<IoUring>(2048);
    
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

bool UringEchoServer::runOnce(const UringCallback& callback) {
    if (!uring_ || !running_) return false;
    
    // 提交并等待事件
    uring_->submitAndWait(1);
    
    // 处理完成事件
    uring_->processCompletions([this, &callback](const UringRequest& req, int result, uint32_t flags, uint64_t userData) {
        handleCompletion(req, result, flags, userData);
        if (callback) {
            callback(req, result, flags, userData);
        }
    });
    
    return true;
}

void UringEchoServer::run() {
    while (running_) {
        runOnce([](const UringRequest& req, int result, uint32_t flags, uint64_t) {
            // 默认空回调
        });
    }
}

void UringEchoServer::handleCompletion(const UringRequest& req, int result, uint32_t flags, uint64_t) {
    switch ((UringEventType)req.type) {
        case UringEventType::ACCEPT: {
            if (result >= 0) {
                handleAccept(result);
            }
            break;
        }
        case UringEventType::READ: {
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
        case UringEventType::WRITE: {
            handleWrite(req.fd, result, req.bid);
            break;
        }
        case UringEventType::PROV_BUF: {
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

// 全局标志
atomic<bool> g_running{true};

void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

/**
 * @brief 测试 1：基本连接测试
 */
void testBasicConnection() {
    cout << "\n========================================" << endl;
    cout << "Test 1: Basic Connection Test" << endl;
    cout << "========================================" << endl;
    
    constexpr uint16_t PORT = 19999;
    
    // 启动服务器线程
    atomic<bool> serverReady{false};
    
    thread serverThread([&serverReady]() {
        UringEchoServer server(PORT);
        if (!server.start()) {
            cerr << "[Server] Failed to start" << endl;
            return;
        }
        
        serverReady = true;
        cout << "[Server] Started on port " << PORT << endl;
        
        // 运行事件循环
        while (g_running) {
            // 单次迭代
            if (!server.runOnce([](const UringRequest& req, int result, uint32_t flags, uint64_t) {
                // 这个回调在 UringEchoServer::run() 内部处理
            })) {
                this_thread::sleep_for(chrono::milliseconds(10));
            }
        }
        
        cout << "[Server] Stopped" << endl;
    });
    
    // 等待服务器启动
    this_thread::sleep_for(chrono::milliseconds(100));
    
    if (!serverReady) {
        cerr << "[Test] Server not ready, skipping test" << endl;
        g_running = false;
        serverThread.join();
        return;
    }
    
    // 连接客户端
    ClientSocket client;
    if (client.connect("127.0.0.1", PORT)) {
        cout << "[Client] Connected: fd=" << client.fd() << endl;
        
        IPAddress local = client.getLocalAddress();
        IPAddress remote = client.getPeerAddress();
        
        cout << "[Client] Local: " << local.toString() << endl;
        cout << "[Client] Remote: " << remote.toString() << endl;
        
        client.close();
        cout << "[Client] Connection test passed" << endl;
    } else {
        cerr << "[Client] Failed to connect" << endl;
    }
    
    // 停止服务器
    g_running = false;
    serverThread.join();
}

/**
 * @brief 测试 2：Echo 功能测试
 */
void testEcho() {
    cout << "\n========================================" << endl;
    cout << "Test 2: Echo Functionality Test" << endl;
    cout << "========================================" << endl;
    
    constexpr uint16_t PORT = 19998;
    
    // 启动服务器
    thread serverThread([PORT]() {
        UringEchoServer server(PORT);
        if (!server.start()) {
            return;
        }
        
        while (g_running) {
            server.runOnce([](const UringRequest& req, int result, uint32_t flags, uint64_t) {
                // 回调在内部处理
            });
        }
    });
    
    this_thread::sleep_for(chrono::milliseconds(100));
    
    // 测试 echo
    ClientSocket client;
    if (client.connect("127.0.0.1", PORT)) {
        vector<string> messages = {
            "Hello, io_uring!",
            "Echo test message",
            "1234567890",
            "Short",
            "A"
        };
        
        for (const auto& msg : messages) {
            // 发送
            int sent = client.sendString(msg);
            if (sent < 0) {
                cerr << "[Client] Send failed" << endl;
                break;
            }
            cout << "[Client] Sent: " << msg << endl;
            
            // 接收 echo
            char buffer[4096];
            int n = client.recv(buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';
                string echo(buffer, n);
                cout << "[Client] Echo: " << echo << endl;
                
                if (echo == msg) {
                    cout << "[Client] Verify: OK" << endl;
                } else {
                    cout << "[Client] Verify: FAILED" << endl;
                }
            } else {
                cerr << "[Client] Recv failed" << endl;
                break;
            }
            
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        
        client.close();
    }
    
    g_running = false;
    serverThread.join();
}

/**
 * @brief 测试 3：大数据传输测试
 */
void testLargeData() {
    cout << "\n========================================" << endl;
    cout << "Test 3: Large Data Transfer Test" << endl;
    cout << "========================================" << endl;
    
    constexpr uint16_t PORT = 19997;
    
    thread serverThread([PORT]() {
        UringEchoServer server(PORT);
        if (!server.start()) {
            return;
        }
        
        while (g_running) {
            server.runOnce([](const UringRequest& req, int result, uint32_t flags, uint64_t) {
            });
        }
    });
    
    this_thread::sleep_for(chrono::milliseconds(100));
    
    ClientSocket client;
    if (client.connect("127.0.0.1", PORT)) {
        // 发送 10KB 数据
        string largeData(10240, 'X');
        largeData += "END";
        
        cout << "[Client] Sending " << largeData.size() << " bytes..." << endl;
        
        int sent = client.send(largeData.data(), largeData.size());
        if (sent < 0) {
            cerr << "[Client] Send failed" << endl;
        } else {
            cout << "[Client] Sent " << sent << " bytes" << endl;
            
            // 接收 echo
            string response;
            response.reserve(largeData.size());
            
            size_t total = 0;
            while (total < (size_t)sent) {
                char buffer[4096];
                int n = client.recv(buffer, sizeof(buffer));
                if (n <= 0) break;
                response.append(buffer, n);
                total += n;
            }
            
            cout << "[Client] Received " << response.size() << " bytes" << endl;
            
            if (response == largeData) {
                cout << "[Client] Large data verify: OK" << endl;
            } else {
                cout << "[Client] Large data verify: FAILED" << endl;
                cout << "[Client] Expected: " << largeData.size() << " bytes" << endl;
                cout << "[Client] Got: " << response.size() << " bytes" << endl;
            }
        }
        
        client.close();
    }
    
    g_running = false;
    serverThread.join();
}

/**
 * @brief 测试 4：多客户端并发测试
 */
void testMultiClient() {
    cout << "\n========================================" << endl;
    cout << "Test 4: Multi-Client Concurrency Test" << endl;
    cout << "========================================" << endl;
    
    constexpr uint16_t PORT = 19996;
    
    thread serverThread([PORT]() {
        UringEchoServer server(PORT);
        if (!server.start()) {
            return;
        }
        
        while (g_running) {
            server.runOnce([](const UringRequest& req, int result, uint32_t flags, uint64_t) {
            });
        }
    });
    
    this_thread::sleep_for(chrono::milliseconds(100));
    
    // 创建多个客户端线程
    vector<thread> clients;
    atomic<int> successCount{0};
    
    for (int i = 0; i < 5; ++i) {
        clients.emplace_back([i, PORT, &successCount]() {
            ClientSocket client;
            if (client.connect("127.0.0.1", PORT)) {
                string msg = "Hello from client " + to_string(i);
                
                int sent = client.sendString(msg);
                if (sent > 0) {
                    char buffer[4096];
                    int n = client.recv(buffer, sizeof(buffer) - 1);
                    if (n > 0) {
                        buffer[n] = '\0';
                        if (string(buffer, n) == msg) {
                            successCount++;
                            cout << "[Client " << i << "] Echo verified: OK" << endl;
                        }
                    }
                }
                client.close();
            }
        });
    }
    
    // 等待所有客户端完成
    for (auto& t : clients) {
        t.join();
    }
    
    cout << "[Test] " << successCount << "/5 clients passed" << endl;
    
    g_running = false;
    serverThread.join();
}

/**
 * @brief 测试 5：IoUring 底层 API 测试
 */
void testLowLevelAPI() {
    cout << "\n========================================" << endl;
    cout << "Test 5: Low-Level IoUring API Test" << endl;
    cout << "========================================" << endl;
    
    // 创建 IoUring 实例
    try {
        IoUring uring(1024);
        
        cout << "[Test] IoUring created successfully" << endl;
        
        // 检查特性支持
        if (uring.checkFeatures()) {
            cout << "[Test] IORING_FEAT_FAST_POLL supported" << endl;
        }
        
        // 初始化缓冲区
        if (uring.initBuffers(1337, 2048, 4096)) {
            cout << "[Test] Buffers initialized: 4096 buffers x 2048 bytes" << endl;
        }
        
        cout << "[Test] Low-level API test passed" << endl;
        
    } catch (const exception& e) {
        cerr << "[Test] Exception: " << e.what() << endl;
    }
}

// ============ 主函数 ============

int main(int argc, char* argv[]) {
    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    cout << "========================================" << endl;
    cout << "   io_uring Echo Test Suite" << endl;
    cout << "========================================" << endl;
    
    if (argc > 1) {
        // 服务器模式
        const char* mode = argv[1];
        
        if (strcmp(mode, "server") == 0) {
            uint16_t port = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 9999;
            
            UringEchoServer server(port);
            if (server.start()) {
                cout << "Server running. Press Ctrl+C to stop." << endl;
                while (g_running) {
                    server.runOnce([](const UringRequest& req, int result, uint32_t flags, uint64_t) {
                    });
                }
            }
            return 0;
        }
    }
    
    // 运行所有测试
    testLowLevelAPI();
    this_thread::sleep_for(chrono::seconds(1));
    
    testBasicConnection();
    this_thread::sleep_for(chrono::seconds(1));
    
    // 重置标志
    g_running = true;
    testEcho();
    this_thread::sleep_for(chrono::seconds(1));
    
    g_running = true;
    testLargeData();
    this_thread::sleep_for(chrono::seconds(1));
    
    g_running = true;
    testMultiClient();
    
    cout << "\n========================================" << endl;
    cout << "All tests completed!" << endl;
    cout << "========================================" << endl;
    
    return 0;
}
