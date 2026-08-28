#ifndef VLESS_NET_SOCKET_CPP
#define VLESS_NET_SOCKET_CPP

#include "net/socket.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <iostream>

namespace {

inline SocketError errnoToSocketError(int err) {
    switch (err) {
        case 0:
            return SocketError::None;
        case EACCES:
        case EAFNOSUPPORT:
        case EINVAL:
        case EPROTONOSUPPORT:
            return SocketError::CreateFailed;
        case EADDRINUSE:
        case EADDRNOTAVAIL:
            return SocketError::BindFailed;
        case EOPNOTSUPP:
        case EPROTO:
            return SocketError::ListenFailed;
        case ECONNREFUSED:
        case ENETUNREACH:
        case EHOSTUNREACH:
            return SocketError::ConnectFailed;
        case EBADF:
        case ENOTSOCK:
            return SocketError::Unknown;
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return SocketError::Timeout;
        case ESHUTDOWN:
        case ECONNRESET:
            return SocketError::PeerClosed;
        default:
            return SocketError::Unknown;
    }
}

}  // namespace

// ============ Socket ============

Socket::Socket() : fd_(-1) {}

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool Socket::setNonBlocking(bool enable) {
    if (fd_ < 0) return false;
    
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return false;
    
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    
    return fcntl(fd_, F_SETFL, flags) == 0;
}

bool Socket::setReuseAddr(bool enable) {
    if (fd_ < 0) return false;
    
    int opt = enable ? 1 : 0;
    return setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
}

bool Socket::setRecvTimeout(int ms) {
    if (fd_ < 0) return false;
    
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool Socket::setSendTimeout(int ms) {
    if (fd_ < 0) return false;
    
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    
    return setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

int Socket::send(const void* data, size_t len) {
    if (fd_ < 0) return -1;
    return ::send(fd_, data, len, 0);
}

int Socket::recv(void* buf, size_t len) {
    if (fd_ < 0) return -1;
    return ::recv(fd_, buf, len, 0);
}

IPAddress Socket::getPeerAddress() const {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    
    if (getpeername(fd_, (struct sockaddr*)&addr, &addrlen) < 0) {
        return IPAddress();
    }
    
    return IPAddress(inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
}

IPAddress Socket::getLocalAddress() const {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    
    if (getsockname(fd_, (struct sockaddr*)&addr, &addrlen) < 0) {
        return IPAddress();
    }
    
    return IPAddress(inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ============ ServerSocket ============

ServerSocket::ServerSocket() : Socket() {}

bool ServerSocket::listen(uint16_t port, int backlog) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }
    
    // 设置地址复用
    setReuseAddr(true);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    if (::listen(fd_, backlog) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    return true;
}

std::unique_ptr<Socket> ServerSocket::accept() {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    
    int clientFd = ::accept(fd_, (struct sockaddr*)&addr, &addrlen);
    if (clientFd < 0) {
        return nullptr;
    }
    
    return std::make_unique<Socket>(clientFd);
}

std::unique_ptr<Socket> ServerSocket::accept(IPAddress& clientAddr) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    
    int clientFd = ::accept(fd_, (struct sockaddr*)&addr, &addrlen);
    if (clientFd < 0) {
        return nullptr;
    }
    
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    clientAddr = IPAddress(ip, ntohs(addr.sin_port));
    
    return std::make_unique<Socket>(clientFd);
}

// ============ ClientSocket ============

ClientSocket::ClientSocket() : Socket() {}

bool ClientSocket::connect(const std::string& ip, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    return ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0;
}

bool ClientSocket::connect(const std::string& ip, uint16_t port, int timeoutMs) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }
    
    // 设置非阻塞
    setNonBlocking(true);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    int ret = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    if (ret == 0) {
        // 连接成功
        return true;
    }
    
    // 等待连接完成
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(fd_, &writeSet);
    
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    
    ret = select(fd_ + 1, nullptr, &writeSet, nullptr, &tv);
    if (ret <= 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    // 检查连接是否真的成功
    int error = 0;
    socklen_t errorLen = sizeof(error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &errorLen) < 0 || error != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    return true;
}

int ClientSocket::sendString(const std::string& str) {
    return send(str.data(), str.size());
}

std::string ClientSocket::recvLine(size_t maxLen) {
    std::string result;
    result.reserve(256);
    
    char c;
    while (result.size() < maxLen) {
        int n = recv(&c, 1);
        if (n <= 0) {
            break;
        }
        if (c == '\n') {
            // 去掉 \r\n
            if (!result.empty() && result.back() == '\r') {
                result.pop_back();
            }
            break;
        }
        result += c;
    }
    
    return result;
}

std::string ClientSocket::recvExactly(size_t len) {
    std::string result;
    result.resize(len);
    
    size_t total = 0;
    while (total < len) {
        int n = recv(&result[total], len - total);
        if (n <= 0) {
            result.resize(total);
            return result;
        }
        total += n;
    }
    
    return result;
}

// ============ SocketUtil ============

namespace SocketUtil {

const char* errorToString(SocketError err) {
    switch (err) {
        case SocketError::None:          return "No error";
        case SocketError::CreateFailed:  return "Socket create failed";
        case SocketError::BindFailed:    return "Bind failed";
        case SocketError::ListenFailed: return "Listen failed";
        case SocketError::ConnectFailed:return "Connect failed";
        case SocketError::AcceptFailed: return "Accept failed";
        case SocketError::SendFailed:   return "Send failed";
        case SocketError::RecvFailed:   return "Recv failed";
        case SocketError::SetOptionFailed: return "Set socket option failed";
        case SocketError::Timeout:      return "Operation timeout";
        case SocketError::PeerClosed:   return "Peer closed connection";
        case SocketError::Unknown:      return "Unknown error";
        default:                        return "Unknown error";
    }
}

SocketError getLastError() {
    return errnoToSocketError(errno);
}

IPAddress toIPAddress(const sockaddr* addr, socklen_t addrlen) {
    if (addr == nullptr || addrlen < sizeof(sockaddr_in)) {
        return IPAddress();
    }
    
    const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    return IPAddress(ip, ntohs(in->sin_port));
}

std::string formatIPv4(uint32_t ip) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             (ip >> 0) & 0xFF,
             (ip >> 8) & 0xFF,
             (ip >> 16) & 0xFF,
             (ip >> 24) & 0xFF);
    return buf;
}

uint32_t parseIPv4(const std::string& ip) {
    uint32_t result = 0;
    uint8_t* parts = reinterpret_cast<uint8_t*>(&result);
    
    // 按 . 分割并转换
    int part = 0;
    uint32_t value = 0;
    for (char c : ip) {
        if (c == '.') {
            parts[part++] = static_cast<uint8_t>(value);
            value = 0;
            if (part >= 4) break;
        } else if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
        }
    }
    if (part < 4) {
        parts[part] = static_cast<uint8_t>(value);
    }
    
    return result;
}

}  // namespace SocketUtil

#endif  // VLESS_NET_SOCKET_CPP
