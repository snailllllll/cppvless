#ifndef VMESS_SERVER_EVENT_LOOP_RUNNER_H
#define VMESS_SERVER_EVENT_LOOP_RUNNER_H

#include "server/event_loop.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vmess {
namespace server {

/**
 * @brief 多 EventLoop 运行器（服务端 / 客户端共用）
 *
 * 收敛两个入口（vmess_server / vmess_client）中重复的：
 *   - 信号处理（SIGINT/SIGTERM → 停止所有 loop）
 *   - 每个 loop 一个线程启动、join
 *   - worker 异常捕获与统一错误传播
 */

/// 注册 SIGINT/SIGTERM 处理；收到信号后停止 loops 中的所有 EventLoop
void installSignalHandler(const std::vector<EventLoop*>& loops);

/**
 * @brief 启动一组 EventLoop，阻塞直到全部退出。
 * @param loops  事件循环集合（按 ports 一一对应）
 * @param ports  每个 loop 的监听端口
 * @param reusePort 为 true 时多个 loop 用 SO_REUSEPORT 共享端口
 * @param errorOut 非空时接收 worker 异常原因
 * @return 0 成功；非 0 表示至少一个 worker 异常退出
 */
int runEventLoops(std::vector<std::unique_ptr<EventLoop>>& loops,
                  const std::vector<uint16_t>& ports,
                  bool reusePort,
                  std::string* errorOut = nullptr);

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_EVENT_LOOP_RUNNER_H
