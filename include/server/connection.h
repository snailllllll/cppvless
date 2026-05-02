#ifndef VMESS_SERVER_CONNECTION_H
#define VMESS_SERVER_CONNECTION_H

#include "net/io_uring.h"

namespace vmess {
namespace server {

/**
 * @brief 连接抽象接口
 * 
 * 主循环只认识这个接口，不认识任何具体协议。
 * 未来支持 VMess、SOCKS5 等协议时，只需实现新的 Connection 子类。
 */
class Connection {
public:
    virtual ~Connection() = default;

    /**
     * @brief 准备 I/O 操作（填 SQE）
     * @param uring io_uring 实例
     * 
     * 主循环每轮迭代前调用，让连接决定需要哪些 I/O。
     */
    virtual void prepareIO(net::IoUring& uring) = 0;

    /**
     * @brief I/O 完成回调
     * @param fd 完成事件的 fd
     * @param result 结果（>0 成功，<=0 出错/关闭）
     * @param type 事件类型（READ/WRITE 等）
     * 
     * 主循环在 CQE 到达时调用。
     */
    virtual void onIOComplete(int fd, int result, net::UringEventType type) = 0;

    /**
     * @brief 连接是否已关闭
     */
    virtual bool isClosed() const = 0;

    /**
     * @brief 获取主 fd（用于连接查找）
     */
    virtual int primaryFd() const = 0;

    /**
     * @brief 判断是否持有该 fd（支持 target fd 反向查找）
     */
    virtual bool hasFd(int fd) const { return fd == primaryFd(); }

    /**
     * @brief 是否需要重新 prepareIO()（状态刚变化，如 HANDSHAKE→RELAY）
     */
    virtual bool needsPrepare() const { return false; }

    /**
     * @brief 清除 needsPrepare 标志
     */
    virtual void clearNeedsPrepare() {}
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_CONNECTION_H
