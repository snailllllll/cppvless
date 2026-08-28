#ifndef VLESS_NET_IO_URING_H
#define VLESS_NET_IO_URING_H

#include <liburing.h>
#include <cstdint>
#include <functional>

/**
 * @file io_uring.h
 * @brief io_uring 异步 I/O 封装
 * 
 * 职责划分（协程架构下 IoUring 只做三件事）：
 * 1. 初始化 io_uring 实例
 * 2. 批量提交 SQE（submitAndWait）
 * 3. 收割 CQE（processCompletions）
 * 
 * SQE 的填写与 user_data 编码由调用方（coro/uring_awaitable.h）直接
 * 通过 ring() 操作，user_data 语义不归本层管理。
 */

// 前置声明
struct io_uring;

namespace vless {
namespace net {

/**
 * @brief io_uring 事件类型（与 user_data 编码共用）
 */
enum class UringEventType : uint16_t {
    ACCEPT = 0,
    READ = 1,
    WRITE = 2
};

/**
 * @brief io_uring 完成事件回调
 * 
 * 不解析 user_data 语义，只透传原始 CQE 字段，由调用方按自己的编码解码。
 */
using UringCallback = std::function<void(int result, uint32_t flags, uint64_t userData)>;

/**
 * @brief io_uring 异步 I/O 处理器（薄封装）
 * 
 * 只负责 ring 生命周期与 SQE 提交 / CQE 收割。
 * 具体操作（accept/recv/send 等）由协程层通过 ring() 直接填写 SQE。
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

    // ============ SQE 提交 ============

    /**
     * @brief 提交所有待处理的 SQE 并等待指定数量的完成事件
     * @param waitNum 等待的完成事件数量
     * @return 提交的 SQE 数量
     */
    int submitAndWait(unsigned waitNum = 1);

    // ============ CQE 收割 ============

    /**
     * @brief 处理所有已完成的 CQE
     * @param callback 完成事件回调函数
     */
    void processCompletions(const UringCallback& callback);

    /**
     * @brief 获取底层 io_uring 指针（协程层直接填写 SQE 用）
     */
    struct io_uring* ring() { return ring_; }

private:
    struct io_uring* ring_;
};

}  // namespace net
}  // namespace vless

#endif  // VLESS_NET_IO_URING_H
