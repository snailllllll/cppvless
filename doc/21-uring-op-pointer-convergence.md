# io_uring 桥接层收敛：user_data 指针化 + cancelFd 取消模型

> 状态：已实施（2026-08-15）
> 关联：`include/coro/uring_awaitable.h`、`src/server/event_loop.cpp`、客户端关闭路径
> 背景：与 `cpp-http-server`（自 vmess 分叉的 http-server 模块）技术底座收敛

## 1. 动机

`cpp-http-server` 与 vmess 共用相同技术底座（io_uring + C++20 协程），但 CQE → 协程的
桥接机制走了两条不同的路线：

| | vmess（旧） | cpp-http-server |
|---|---|---|
| user_data 内容 | 编码（bit63 标志 + type + fd） | 裸指针（操作对象地址） |
| 找协程方式 | `CoroutineRegistry` 查表（fd+type 键） | 指针直达，零查表 |
| 完成行为 | `takeAndResume` 集中 switch(type) | 每个 op 自带 `static complete` 回调 |
| 取消机制 | `eraseAll` + `!done()` 防御 | `PendingUringOps::cancelFd` 显式作废 |

收敛方向：vmess 采用 cpp-http-server 的**指针直分发 + cancelFd** 模型。

## 2. 新机制

### 2.1 user_data = 操作对象指针

每个异步操作（recv/send/connect/accept/shutdown/recvmsg/sendmsg）在 awaitable 内持有一个
操作上下文 `op_`，SQE 的 `user_data` 直接填 `&op_`：

```cpp
sqe->user_data = reinterpret_cast<uint64_t>(&op_);
```

CQE 到达时直接还原指针，经 `UringOp::completeFromCqe` 调用该 op 的完成回调，**无需查表**。

### 2.2 UringOp 基类（函数指针多态）

```cpp
struct UringOp {
    using CompleteFn = void (*)(UringOp* self, int res, uint32_t flags) noexcept;
    CompleteFn onComplete = nullptr;
    std::coroutine_handle<> handle{};
    int fd = -1;

    static void completeFromCqe(uint64_t userData, int res, uint32_t flags) {
        if (userData == 0) return;
        auto* op = reinterpret_cast<UringOp*>(userData);
        if (!op || !op->onComplete) return;
        op->onComplete(op, res, flags);
    }
};
```

子类（`RecvOp` / `WriteOp` / `AcceptOp`）各自实现 `static complete`：写结果 → 从
`PendingUringOps` 移除 → 置空回调 → resume 协程。

### 2.3 PendingUringOps（取消模型，thread_local）

```cpp
class PendingUringOps {
    void add(UringOp* op);          // 登记进行中的操作（按 fd 索引）
    void remove(UringOp* op);       // 完成时移除
    void cancelFd(int fd);          // 连接关闭：作废该 fd 所有挂起操作
};
```

`cancelFd(fd)` 遍历该 fd 的挂起 op，把 `onComplete` 置空、`handle` 置空——CQE 迟到时
`completeFromCqe` 看到空回调直接忽略，**不会 resume 已销毁的协程帧**。

⚠️ 与 cpp-http-server 的关键差异：它用普通 `static` 单例（单线程）；vmess 是多 worker
线程，**必须保持 `thread_local`**，避免跨线程误 resume。

## 3. 改动清单

| 文件 | 改动 |
|---|---|
| `include/coro/uring_awaitable.h` | 删 `CoroutineRegistry` + 编码函数（~95 行）；新增 `UringOp`/`PendingUringOps`/`RecvOp`/`WriteOp`/`AcceptOp`（~105 行）；7 个 Async* 类改造为 op 指针模式 |
| `src/server/event_loop.cpp` | `processCompletions` 回调改为 `UringOp::completeFromCqe`，去掉 fd/type 解码 |
| `src/server/vless_connection.cpp` | `eraseAll` → `cancelFd` |
| `src/client/socks5_connection.cpp` | `eraseAll` → `cancelFd` |
| `src/client/socks5_udp_relay.cpp` | `eraseAll` → `cancelFd`（4 处） |

## 4. 收益

1. **CQE 分发零查表**：user_data 即指针，直接函数指针分发；
2. **取消模型更严谨**：`cancelFd` 显式作废挂起操作，比"eraseAll + `!done()` 防御"更早切断指针；
3. **与 cpp-http-server 完全收敛**：两侧可互相移植代码与修复。

## 5. 遗留注意

- UDP 的 `AsyncRecvFrom`/`AsyncSendTo` 在 cpp-http-server 中不存在，其 op 需保留
  `sockaddr_storage`/`iovec`/`msghdr` 状态；
- 文档 `doc/architecture.md` 中 `CoroutineRegistry` 相关描述待同步。
