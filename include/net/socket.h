#ifndef VMESS_NET_SOCKET_H
#define VMESS_NET_SOCKET_H

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

#include <sys/socket.h>   // 提供 socklen_t
#include <netinet/in.h>   // 提供 struct sockaddr_in

struct sockaddr_in;
struct sockaddr;

/**
 * @brief Socket 错误类型
 */
enum class SocketError {
    None = 0,
    CreateFailed,
    BindFailed,
    ListenFailed,
    ConnectFailed,
    AcceptFailed,
    SendFailed,
    RecvFailed,
    SetOptionFailed,
    Timeout,
    PeerClosed,
    Unknown
};

/**
 * @brief IP 地址结构
 */
struct IPAddress {
    std::string ip;
    uint16_t port;

    IPAddress() : ip("0.0.0.0"), port(0) {}
    IPAddress(const std::string& ip, uint16_t port) : ip(ip), port(port) {}
    
    std::string toString() const {
        return ip + ":" + std::to_string(port);
    }
};

/**
 * @brief Socket 基类封装
 */
class Socket {
public:
    Socket();
    Socket(int fd);
    virtual ~Socket();

    // 禁用拷贝
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // 支持移动
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    /**
     * @brief 获取文件描述符
     */
    int fd() const { return fd_; }

    /**
     * @brief 获取是否有效
     */
    bool valid() const { return fd_ >= 0; }

    /**
     * @brief 设置非阻塞模式
     */
    bool setNonBlocking(bool enable);

    /**
     * @brief 设置地址复用
     */
    bool setReuseAddr(bool enable);

/**
 * @brief 设置超时
     */
    bool setRecvTimeout(int ms);
    bool setSendTimeout(int ms);

    /**
     * @brief 发送数据
     */
    int send(const void* data, size_t len);

    /**
     * @brief 接收数据
     */
    int recv(void* buf, size_t len);

    /**
     * @brief 获取对端地址
     */
    IPAddress getPeerAddress() const;

    /**
     * @brief 获取本地地址
     */
    IPAddress getLocalAddress() const;

    /**
     * @brief 关闭连接
     */
    void close();

    /**
     * @brief 转换为原始 fd
     */
    operator int() const { return fd_; }

protected:
    int fd_;
};

/**
 * @brief TCP 服务器 Socket
 */
class ServerSocket : public Socket {
public:
    ServerSocket();
    
    /**
     * @brief 绑定并监听端口
     * @param port 端口号
     * @param backlog 连接队列大小
     */
    bool listen(uint16_t port, int backlog = 128);

    /**
     * @brief 接受客户端连接
     * @return 新的客户端 socket，失败返回 nullptr
     */
    std::unique_ptr<Socket> accept();

    /**
     * @brief 接受并获取客户端地址
     */
    std::unique_ptr<Socket> accept(IPAddress& clientAddr);
};

/**
 * @brief TCP 客户端 Socket
 */
class ClientSocket : public Socket {
public:
    ClientSocket();

    /**
     * @brief 连接到服务器
     * @param ip 服务器 IP
     * @param port 服务器端口
     */
    bool connect(const std::string& ip, uint16_t port);

    /**
     * @brief 连接到服务器（带超时）
     */
    bool connect(const std::string& ip, uint16_t port, int timeoutMs);

    /**
     * @brief 发送字符串
     */
    int sendString(const std::string& str);

    /**
     * @brief 接收一行数据（以 \n 结尾）
     */
    std::string recvLine(size_t maxLen = 1024);

    /**
     * @brief 接收指定字节数
     */
    std::string recvExactly(size_t len);
};

/**
 * @brief Socket 工具函数
 */
namespace SocketUtil {

/**
 * @brief 获取 Socket 错误描述
 */
const char* errorToString(SocketError err);

/**
 * @brief 获取最近一次系统错误
 */
SocketError getLastError();

/**
 * @brief 获取当前线程的最后一个错误码
 */
inline SocketError lastError() {
    return getLastError();
}

/**
 * @brief 将 sockaddr 转换为 IPAddress
 */
IPAddress toIPAddress(const sockaddr* addr, socklen_t addrlen);

/**
 * @brief 格式化 IP 地址
 */
std::string formatIPv4(uint32_t ip);

/**
 * @brief 解析 IPv4 地址字符串
 */
uint32_t parseIPv4(const std::string& ip);

}  // namespace SocketUtil

#endif  // VMESS_NET_SOCKET_H
