#ifndef VLESS_SERVER_CONNECTION_H
#define VLESS_SERVER_CONNECTION_H

namespace vless {
namespace server {

/**
 * @brief 事件循环连接抽象接口
 *
 * 主循环只认识这个接口，不认识任何具体协议。
 * VLESS 服务端（VlessConnection）与 SOCKS5 客户端（Socks5Connection）
 * 都实现该接口，通过工厂创建，因此 EventLoop 可被两端共用。
 *
 * 生命周期：
 *   - acceptLoop 通过 ConnectionFactory 创建连接并调用 start()
 *   - start() 启动协程状态机
 *   - 主循环每轮调用 cleanupClosedConnections()，回收 isClosed() 的连接
 */
class EventLoopConnection {
public:
    virtual ~EventLoopConnection() = default;

    /// 启动会话状态机（协程）
    virtual void start() = 0;

    /// 连接是否已关闭（可被主循环回收）
    virtual bool isClosed() const = 0;

    /// 主 fd（用于连接查找）
    virtual int primaryFd() const = 0;

    /// 判断是否持有该 fd（支持 target fd 反向查找）
    virtual bool hasFd(int fd) const { return fd == primaryFd(); }
};

} // namespace server
} // namespace vless

#endif // VLESS_SERVER_CONNECTION_H
