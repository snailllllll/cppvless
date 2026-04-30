#ifndef VMESS_NET_IO_URING_H
#define VMESS_NET_IO_URING_H

#include <liburing.h>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include <netinet/in.h>
#include "socket.h"

/**
 * @file io_uring.h
 * @brief io_uring 异步 I/O 封装
 * 
 * 设计理念：
 * 1. 与 Socket 封装解耦，通过 fd 操作
 * 2. 提供底层 SQE 提交接口
 * 3. 支持 Buffer Selection 实现零拷贝
 * 4. 未来可扩展 C++20 协程接口
 */

// 前置声明
struct io_uring;

namespace vmess {
namespace net {

/**
 * @brief io_uring 事件类型
 */
enum class UringEventType : uint16_t {
    ACCEPT = 0,
    READ = 1,
    WRITE = 2,
    PROV_BUF = 3,
    TIMEOUT = 4
};

/**
 * @brief io_uring 请求上下文
 * 
 * 通过 user_data 传递给内核，完成时在 CQE 中返回
 */
struct UringRequest {
    int fd;              // 文件描述符
    uint16_t type;       // 事件类型 (UringEventType)
    uint16_t bid;        // 缓冲区 ID (用于 Buffer Selection)
    
    // 转换为 64 位整数存储到 user_data
    uint64_t toUserData() const {
        return (uint64_t)fd | ((uint64_t)type << 32) | ((uint64_t)bid << 48);
    }
    
    static UringRequest fromUserData(uint64_t data) {
        return {
            .fd = (int)(data & 0xFFFFFFFF),
            .type = (uint16_t)((data >> 32) & 0xFFFF),
            .bid = (uint16_t)((data >> 48) & 0xFFFF)
        };
    }
};

/**
 * @brief io_uring 事件回调
 */
using UringCallback = std::function<void(const UringRequest& req, int result, uint32_t flags)>;

/**
 * @brief io_uring 异步 I/O 处理器
 * 
 * 封装 io_uring 的初始化、SQE 提交、CQE 处理
 */
class IoUring {
public:
    /**
     * @brief 构造函数
     * @param entries SQ/CQ 队列大小
     */
    explicit IoUring(unsigned int entries = 2048);
    
    ~IoUring();
    
    // 禁用拷贝
    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;
    
    // 支持移动
    IoUring(IoUring&& other) noexcept;
    IoUring& operator=(IoUring&& other) noexcept;
    
    /**
     * @brief 检查内核是否支持所需特性
     * @return true 如果支持 IORING_FEAT_FAST_POLL
     */
    bool checkFeatures() const;
    
    /**
     * @brief 初始化缓冲区组（用于 Buffer Selection）
     * @param groupId 缓冲区组 ID
     * @param bufferSize 每个缓冲区大小
     * @param bufferCount 缓冲区数量
     * @return true 如果初始化成功
     */
    bool initBuffers(unsigned groupId, size_t bufferSize, size_t bufferCount);
    
    /**
     * @brief 回收单个缓冲区
     * @param groupId 缓冲区组 ID
     * @param bid 缓冲区 ID
     */
    void provideBuffer(unsigned groupId, uint16_t bid);
    
    // ============ 准备 SQE（不提交）============
    // 这些方法只是将操作添加到 SQ 缓冲区，需要调用 submitAll() 或 submitAndWait() 才会真正提交给内核
    
    /**
     * @brief 准备 accept 操作（不提交）
     * @param listenFd 监听 socket fd
     * @param clientAddr 客户端地址（输出）
     * @param addrLen 地址长度（输入输出）
     * @param flags SQE 标志
     * @return true 如果准备成功
     */
    bool prepareAccept(int listenFd, struct sockaddr* clientAddr, 
                      socklen_t* addrLen, unsigned flags = 0);
    
    /**
     * @brief 准备 recv 操作（使用 Buffer Selection，不提交）
     * @param fd 客户端 socket fd
     * @param groupId 缓冲区组 ID
     * @param bufferSize 缓冲区大小
     * @param flags SQE 标志
     * @return true 如果准备成功
     */
    bool prepareRecv(int fd, unsigned groupId, size_t bufferSize, unsigned flags = IOSQE_BUFFER_SELECT);
    
    /**
     * @brief 准备 recv 操作（指定缓冲区，不提交）
     * @param fd 客户端 socket fd
     * @param buf 接收缓冲区
     * @param len 缓冲区大小
     * @param flags SQE 标志
     * @return true 如果准备成功
     */
    bool prepareRecv(int fd, void* buf, size_t len, unsigned flags = 0);
    
    /**
     * @brief 准备 send 操作（不提交）
     * @param fd 客户端 socket fd
     * @param buf 发送缓冲区
     * @param len 数据长度
     * @param flags SQE 标志
     * @return true 如果准备成功
     */
    bool prepareSend(int fd, const void* buf, size_t len, unsigned flags = 0);
    
    /**
     * @brief 准备 shutdown 操作（不提交）
     * @param fd socket fd
     * @return true 如果准备成功
     */
    bool prepareShutdown(int fd);
    
    /**
     * @brief 准备 close 操作（不提交）
     * @param fd socket fd
     * @return true 如果准备成功
     */
    bool prepareClose(int fd);
    
    // ============ 提交 SQE 到内核 ============
    
    /**
     * @brief 提交所有待处理的 SQE 到内核（不等待完成）
     * @return 提交的 SQE 数量
     */
    int submitAll();
    
    /**
     * @brief 提交所有待处理的 SQE 并等待指定数量的完成事件
     * @param waitNum 等待的完成事件数量
     * @return 提交的 SQE 数量
     */
    int submitAndWait(unsigned waitNum = 1);
    
    // ============ 事件循环 ============
    
    /**
     * @brief 处理所有已完成的 CQE
     * @param callback 完成事件回调函数
     */
    void processCompletions(const UringCallback& callback);
    
    /**
     * @brief 运行事件循环（单次迭代）
     * @param callback 完成事件回调函数
     * @return true 如果处理了事件
     */
    bool runOnce(const UringCallback& callback);
    
    /**
     * @brief 获取缓冲区指针
     * @param bid 缓冲区 ID
     * @return 缓冲区指针
     */
    char* getBuffer(uint16_t bid);
    
    /**
     * @brief 获取缓冲区 ID
     * @param buffer 缓冲区指针
     * @return 缓冲区 ID
     */
    uint16_t getBufferId(const char* buffer) const;
    
    /**
     * @brief 获取底层 io_uring 指针（高级用法）
     */
    struct io_uring* ring() { return ring_; }
    
    /**
     * @brief 获取缓冲区大小
     */
    size_t bufferSize() const { return bufferSize_; }
    
    /**
     * @brief 获取缓冲区数量
     */
    size_t bufferCount() const { return bufferCount_; }

private:
    struct io_uring* ring_;
    char* buffers_;              // 预分配的缓冲区数组
    size_t bufferSize_;          // 每个缓冲区大小
    size_t bufferCount_;         // 缓冲区数量
    unsigned groupId_;           // 缓冲区组 ID
    
    /**
     * @brief 填充 UringRequest 到 SQE 的 user_data
     */
    void setUserData(struct io_uring_sqe* sqe, int fd, 
                     UringEventType type, uint16_t bid = 0);
};

}  // namespace net
}  // namespace vmess

#endif  // VMESS_NET_IO_URING_H
