/**
 * @file socket_echo_test.cpp
 * @brief Socket 封装测试：Echo 服务器 + 客户端
 * 
 * 测试功能：
 * 1. 启动监听
 * 2. 客户端连接时输出 fd 和 ip
 * 3. Echo 测试：服务器回显客户端发送的数据
 */

#include "net/socket.h"
#include <iostream>
#include <cstring>
#include <csignal>
#include <thread>
#include <atomic>
#include <vector>

using namespace std;

// 全局标志用于优雅退出
atomic<bool> g_running{true};

void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// ============ Echo 服务器 ============

class EchoServer {
public:
    EchoServer(uint16_t port) : port_(port), serverSocket_() {}
    
    bool start() {
        // 创建并监听
        if (!serverSocket_.listen(port_)) {
            cerr << "[Server] Failed to listen on port " << port_ << endl;
            cerr << "[Server] Error: " << SocketUtil::errorToString(SocketUtil::lastError()) << endl;
            return false;
        }
        
        // 设置非阻塞
        serverSocket_.setNonBlocking(true);
        
        cout << "[Server] Listening on port " << port_ << "..." << endl;
        cout << "[Server] Press Ctrl+C to stop" << endl;
        
        return true;
    }
    
    void run() {
        while (g_running) {
            // 接受连接
            IPAddress clientAddr;
            auto client = serverSocket_.accept(clientAddr);
            
            if (!client) {
                this_thread::sleep_for(chrono::milliseconds(10));
                continue;
            }
            
            // 输出客户端信息
            int fd = client->fd();
            cout << "[Server] New connection: fd=" << fd 
                 << ", ip=" << clientAddr.ip 
                 << ", port=" << clientAddr.port << endl;
            
            // 处理 echo 逻辑
            handleClient(std::move(client), clientAddr);
        }
    }
    
    void stop() {
        cout << "[Server] Shutting down..." << endl;
        serverSocket_.close();
    }
    
private:
    void handleClient(unique_ptr<Socket> client, const IPAddress& clientAddr) {
        (void)clientAddr;
        char buffer[4096];
        
        while (g_running) {
            // 接收数据
            int n = client->recv(buffer, sizeof(buffer) - 1);
            
            if (n > 0) {
                buffer[n] = '\0';
                cout << "[Server] Received from fd=" << client->fd() 
                     << ": " << buffer << endl;
                
                // Echo 回显
                int sent = client->send(buffer, n);
                if (sent < 0) {
                    cerr << "[Server] Send failed" << endl;
                    break;
                }
                cout << "[Server] Echoed " << sent << " bytes to fd=" << client->fd() << endl;
            } else if (n == 0) {
                cout << "[Server] Client disconnected: fd=" << client->fd() << endl;
                break;
            } else {
                // 非阻塞模式下没有数据是正常的
                this_thread::sleep_for(chrono::milliseconds(1));
            }
        }
    }
    
    uint16_t port_;
    ServerSocket serverSocket_;
};

// ============ Echo 客户端 ============

class EchoClient {
public:
    EchoClient(const string& serverIp, uint16_t serverPort)
        : serverIp_(serverIp), serverPort_(serverPort), clientSocket_() {}
    
    bool connect() {
        cout << "[Client] Connecting to " << serverIp_ << ":" << serverPort_ << "..." << endl;
        
        if (!clientSocket_.connect(serverIp_, serverPort_)) {
            cerr << "[Client] Failed to connect" << endl;
            cerr << "[Client] Error: " << SocketUtil::errorToString(SocketUtil::lastError()) << endl;
            return false;
        }
        
        IPAddress localAddr = clientSocket_.getLocalAddress();
        IPAddress remoteAddr = clientSocket_.getPeerAddress();
        
        cout << "[Client] Connected: fd=" << clientSocket_.fd() << endl;
        cout << "[Client] Local: " << localAddr.ip << ":" << localAddr.port << endl;
        cout << "[Client] Remote: " << remoteAddr.ip << ":" << remoteAddr.port << endl;
        
        return true;
    }
    
    bool sendAndReceive(const string& msg) {
        // 发送数据
        int sent = clientSocket_.sendString(msg);
        if (sent < 0) {
            cerr << "[Client] Send failed" << endl;
            return false;
        }
        cout << "[Client] Sent: " << msg << " (" << sent << " bytes)" << endl;
        
        // 接收 echo
        char buffer[4096];
        int n = clientSocket_.recv(buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            cout << "[Client] Received echo: " << buffer << " (" << n << " bytes)" << endl;
            
            // 验证是否一致
            if (msg == string(buffer, n)) {
                cout << "[Client] Echo verified: OK" << endl;
            } else {
                cout << "[Client] Echo verified: MISMATCH" << endl;
            }
            return true;
        } else if (n == 0) {
            cout << "[Client] Server closed connection" << endl;
            return false;
        } else {
            cerr << "[Client] Receive failed" << endl;
            return false;
        }
    }
    
    bool sendRaw(const string& data) {
        int sent = clientSocket_.send(data.data(), data.size());
        if (sent < 0) {
            cerr << "[Client] Send raw failed" << endl;
            return false;
        }
        cout << "[Client] Sent " << sent << " bytes" << endl;
        return true;
    }
    
    string recvRaw(size_t expectedLen) {
        string result;
        result.reserve(expectedLen);
        
        size_t total = 0;
        while (total < expectedLen) {
            char buffer[4096];
            int n = clientSocket_.recv(buffer, min(sizeof(buffer), expectedLen - total));
            if (n <= 0) {
                break;
            }
            result.append(buffer, n);
            total += n;
        }
        
        return result;
    }
    
    void close() {
        clientSocket_.close();
    }
    
    int fd() const { return clientSocket_.fd(); }
    
private:
    string serverIp_;
    uint16_t serverPort_;
    ClientSocket clientSocket_;
};

// ============ 测试函数 ============

void testBasicConnection() {
    cout << "\n========================================" << endl;
    cout << "Test 1: Basic Connection Test" << endl;
    cout << "========================================" << endl;
    
    const uint16_t port = 9999;
    EchoServer server(port);
    
    // 启动服务器
    if (!server.start()) {
        return;
    }
    
    // 启动服务器线程
    thread serverThread([&server]() {
        server.run();
    });
    
    // 等待服务器启动
    this_thread::sleep_for(chrono::milliseconds(100));
    
    // 连接多个客户端
    vector<unique_ptr<EchoClient>> clients;
    for (int i = 0; i < 3; ++i) {
        auto client = make_unique<EchoClient>("127.0.0.1", port);
        if (client->connect()) {
            clients.push_back(std::move(client));
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    
    // 关闭客户端
    for (auto& client : clients) {
        client->close();
    }
    
    // 停止服务器
    g_running = false;
    serverThread.join();
}

void testEchoCommunication() {
    cout << "\n========================================" << endl;
    cout << "Test 2: Echo Communication Test" << endl;
    cout << "========================================" << endl;
    
    const uint16_t port = 9998;
    EchoServer server(port);
    
    if (!server.start()) {
        return;
    }
    
    thread serverThread([&server]() {
        server.run();
    });
    
    this_thread::sleep_for(chrono::milliseconds(100));
    
    // 创建客户端并测试 echo
    EchoClient client("127.0.0.1", port);
    if (client.connect()) {
        // 测试多条消息
        vector<string> messages = {
            "Hello, World!",
            "This is a test message",
            "1234567890",
            "中文测试",
            "Mixed: Hello 你好 123"
        };
        
        for (const auto& msg : messages) {
            if (!client.sendAndReceive(msg)) {
                break;
            }
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        
        client.close();
    }
    
    g_running = false;
    serverThread.join();
}

void testLargeData() {
    cout << "\n========================================" << endl;
    cout << "Test 3: Large Data Test" << endl;
    cout << "========================================" << endl;
    
    const uint16_t port = 9997;
    EchoServer server(port);
    
    if (!server.start()) {
        return;
    }
    
    thread serverThread([&server]() {
        server.run();
    });
    
    this_thread::sleep_for(chrono::milliseconds(100));
    
    EchoClient client("127.0.0.1", port);
    if (client.connect()) {
        // 测试大数据
        string largeData(10240, 'A');  // 10KB
        largeData += "END";
        
        cout << "[Client] Sending " << largeData.size() << " bytes..." << endl;
        
        if (client.sendRaw(largeData)) {
            // 接收响应
            string response = client.recvRaw(largeData.size());
            
            cout << "[Client] Received " << response.size() << " bytes" << endl;
            
            if (response == largeData) {
                cout << "[Client] Large data echo verified: OK" << endl;
            } else {
                cout << "[Client] Large data echo verified: FAILED" << endl;
            }
        }
        
        client.close();
    }
    
    g_running = false;
    serverThread.join();
}

// ============ 主函数 ============

int main(int argc, char* argv[]) {
    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    cout << "========================================" << endl;
    cout << "   Socket Echo Test Suite" << endl;
    cout << "========================================" << endl;
    
    if (argc > 1) {
        // 客户端/服务器模式
        const char* mode = argv[1];
        
        if (strcmp(mode, "server") == 0) {
            uint16_t port = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 9999;
            
            EchoServer server(port);
            if (server.start()) {
                server.run();
                server.stop();
            }
            return 0;
        } 
        else if (strcmp(mode, "client") == 0) {
            const char* ip = argc > 2 ? argv[2] : "127.0.0.1";
            uint16_t port = argc > 3 ? static_cast<uint16_t>(atoi(argv[3])) : 9999;
            
            EchoClient client(ip, port);
            if (client.connect()) {
                cout << "\n========== Echo Client ==========" << endl;
                cout << "Connected to " << ip << ":" << port << endl;
                cout << "Commands:" << endl;
                cout << "  - Type any message and press Enter to send" << endl;
                cout << "  - 'quit' or 'exit' to disconnect" << endl;
                cout << "  - 'help' to show this message" << endl;
                cout << "================================\n" << endl;
                
                string line;
                while (g_running) {
                    cout << "> ";
                    cout.flush();  // 确保提示符立即显示
                    
                    if (!getline(cin, line)) {
                        // EOF (Ctrl+D)
                        cout << "\n[Client] EOF received, disconnecting..." << endl;
                        break;
                    }
                    
                    if (line.empty()) {
                        continue;  // 跳过空行
                    }
                    
                    if (line == "quit" || line == "exit") {
                        cout << "[Client] Disconnecting..." << endl;
                        break;
                    }
                    
                    if (line == "help") {
                        cout << "Commands:" << endl;
                        cout << "  - Type any message and press Enter to send" << endl;
                        cout << "  - 'quit' or 'exit' to disconnect" << endl;
                        cout << "  - 'help' to show this message" << endl;
                        continue;
                    }
                    
                    if (!client.sendAndReceive(line)) {
                        cout << "[Client] Connection lost, disconnecting..." << endl;
                        break;
                    }
                }
                client.close();
            }
            return 0;
        }
    }
    
    // 运行所有测试
    testBasicConnection();
    this_thread::sleep_for(chrono::seconds(1));
    
    testEchoCommunication();
    this_thread::sleep_for(chrono::seconds(1));
    
    testLargeData();
    
    cout << "\n========================================" << endl;
    cout << "All tests completed!" << endl;
    cout << "========================================" << endl;
    
    return 0;
}
