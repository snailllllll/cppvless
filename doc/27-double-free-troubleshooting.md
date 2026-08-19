# double-free 崩溃排障全流程记录

> 状态：**已修复并验证**（v0.0.2）
> 日期：2026-08-19
> 关联：`doc/26-benchmark-report.md`（压测综合报告）、`bench/bench_multi.py`
> 摘要：压测期间发现 cpp server 在高频明文建连场景反复 segfault（systemd NRestarts=19），定位为 `VlessConnection` 析构 double-free，修复后 15.7 万连接零崩溃。

---

## 1. 现象

接管压测环境时，例行检查发现：

```bash
$ systemctl show vmess -p NRestarts
NRestarts=19          # ← 异常：服务已被 systemd 反复重启 19 次
```

进一步确认：

- `systemctl is-active vmess` 显示 `active`（被 Restart=always 掩盖了崩溃）
- `dmesg` 中有大量 `vmess_server[...]: segfault`，**崩溃偏移全部相同**（`in vmess_server[2d67a,...]`，error 4）
- 日志模式固定：`clientTask exception: failed to read vless version` 后进程崩溃
- 端口 1080/8848 在 `ss` 中显示为 2 行（2 个 worker SO_REUSEPORT，正常），但 PID 一直在变（进程反复重启）

**核心怀疑点**：崩溃偏移一致 → 确定性 bug，不是随机竞态；且集中在明文链路压测时（连接建立即断开）。

---

## 2. 排查过程

### 2.1 环境确认（两台南京 CVM）

- 被测端 `ins-jbkczrgu` (10.206.16.17)：`vmess_server`（明文 1080 / TLS 8848）、v2ray（明文 8444）、nginx(80)、iperf3(5201)
- 压测端 `ins-5i4t05a8` (10.206.16.15)：Xray 三链路 socks 10881/10882/10883
- 崩溃只在**明文链路**（10883 cpp-plain）压测时高频出现

### 2.2 本地复现（关键一步）

在开发机直接用本地构建的二进制（与线上 md5 一致 `d8dea407...`）复现：

```bash
# 启动本地实例（避免公网探测）
export VLESS_NO_AUTO_HOST=1 VLESS_CONFIG=/tmp/vmess-local/config.json
/data/workspace/vmess/build-docker/src/vmess_server 18080 debug --log-file /tmp/vmess-local/vmess.log &
# 并发建连-立即断开，触发崩溃
python3 bench/bench_crash_local.py 18080 200 5
```

**结果：完美复现。** dmesg 同样出现 `segfault ... in vmess_server[2d67a,...]`，偏移与线上完全一致。

### 2.3 gdb 抓崩溃栈

```bash
ulimit -c unlimited
gdb -q -batch \
  -ex "run 18080 debug --log-file /tmp/vmess-local/vmess.log" \
  -ex "bt 30" \
  -ex "info registers rip rsp rbp" \
  /data/workspace/vmess/build-docker/src/vmess_server
```

崩溃栈：

```
Thread 7 received signal SIGSEGV
#0  0x...167a in vmess::server::VlessConnection::~VlessConnection()
#1  0x...181d in vmess::server::VlessConnection::~VlessConnection()
#2  0x...9b47 in vmess::server::EventLoop::cleanupClosedConnections()
#3  0x...9dea in vmess::server::EventLoop::run()
```

**崩溃点在析构函数内**（`~VlessConnection()+458`），由 `EventLoop::cleanupClosedConnections` 的 `erase` 触发。

### 2.4 反汇编定位

```
2d65f:  mov 0x30(%rbx),%rdi   ; 取 clientStream_ (unique_ptr<net::Stream>)
2d668:  mov (%rdi),%rax        ; 取虚表
2d66b:  call *0x8(%rax)        ; 虚析构 → delete AsyncStream 对象
...
2d66e:  mov 0x28(%rbx),%rdi   ; 取 rawStream_ (unique_ptr<coro::AsyncStream>)
2d677:  mov (%rdi),%rax        ; 取虚表
2d67a:  mov 0x8(%rax),%rax     ; ← 崩溃：读已释放内存的虚表
```

两次析构的是**同一地址**：`clientStream_` 先 delete 了对象，`rawStream_` 再次 delete → **double free**。

---

## 3. 根因分析

### 3.1 构造代码（`src/server/vless_connection.cpp`）

```cpp
VlessConnection::VlessConnection(int clientFd, net::IoUring& uring, ...)
    : clientFd_(clientFd), ...,
      rawStream_(std::make_unique<coro::AsyncStream>(clientFd, uring)),     // 真实对象
      clientStream_(tlsCtx_                                                  // ← 问题
                        ? static_cast<net::Stream*>(
                              new net::TlsStream(*rawStream_, tlsCtx_, true))
                        : static_cast<net::Stream*>(rawStream_.get())),     // 明文：别名！
      stream_(*clientStream_) {}
```

**明文模式（`tlsCtx_==nullptr`）下 `clientStream_` 直接持有 `rawStream_.get()`**——即两个 `unique_ptr` 指向同一对象。

### 3.2 析构顺序（成员按声明逆序）

成员声明顺序：`rawStream_`（0x28）→ `clientStream_`（0x30）→ ...（后略）

析构逆序：**`clientStream_` 先析构 → `delete(AsyncStream)`；随后 `rawStream_` 析构 → 再次 `delete` 同一对象** → double free / UAF。

### 3.3 为什么 TLS 模式不崩

TLS 模式下 `clientStream_ = new TlsStream(...)`（独立堆对象），与 `rawStream_` 是两个对象，两次 delete 各自有效。**因此崩溃全集中在明文链路**——与线上现象吻合。

### 3.4 为什么之前没暴露

- 低频场景（单连接/少量连接）下 double free 未必立即崩（堆内存未复用），只有高频建连-断开才会快速触发
- `Restart=always` 掩盖了崩溃，表现为"服务偶尔失联几秒"

---

## 4. 修复

### 4.1 修复代码

```cpp
VlessConnection::~VlessConnection() {
    // 明文模式（tlsCtx_ == nullptr）下 clientStream_ 仅是 rawStream_ 的别名
    // （构造时赋值为 rawStream_.get()，不拥有所有权）。成员按声明逆序析构，
    // clientStream_ 先于 rawStream_ 析构，若不先 release 会 double free：
    //   clientStream_ → delete(AsyncStream) → rawStream_ → delete(AsyncStream) 两次
    if (tlsCtx_ == nullptr) {
        clientStream_.release();
    }
    doClose();
}
```

**本质**：明确所有权——明文下 `clientStream_` 不拥有对象，析构前 `release()` 交给 `rawStream_` 唯一释放。

### 4.2 修复的正确性

- TLS 模式：`clientStream_` 拥有 `TlsStream`，不 release，正常析构
- 明文模式：release 后 `clientStream_` 不再 delete，`rawStream_` 释放唯一对象
- 修复后成员析构仍安全：`stream_`（`UringBufferedStream`）持有的是 `clientStream_` 的**引用**，先于两者析构，不参与所有权

---

## 5. 验证

### 5.1 本地复现脚本验证

```bash
# 修复后，本地 3 轮各 5s、200 并发建连-断开
python3 bench/bench_crash_local.py 18080 200 5
# 输出: done: ok=51924 rate=8659/s   （修复前 588 连接即崩）
#       done: ok=53947 rate=8953/s
#       done: ok=53446 rate=8740/s
```

- 修复前：4s 内 588 连接就崩溃
- 修复后：3 轮共 ~15.7 万连接**零崩溃**，且速率从 ~129/s 飙升至 ~8700/s（不再反复重启）

### 5.2 线上部署验证

```bash
# 部署修复版
sudo cp /tmp/vmess_server_fixed /usr/local/bin/vmess_server
sudo systemctl restart vmess
systemctl show vmess -p NRestarts   # → NRestarts=0

# 完整压测（7 轮 L1 + 7 轮 L2）全程
sudo dmesg -T | grep vmess_server   # 无新 segfault
journalctl -u vmess | grep ERROR    # 仅正常握手失败日志（建连-断开场景预期行为）
```

- 修复前 `NRestarts=19`；修复后压测全程 `NRestarts=0`、无 segfault

### 5.3 性能回归（见 `doc/26-benchmark-report.md` §6）

- L1 连接建立：cpp 由 ~3,000 提升至 **3,910 conn/s**（+31%），与 go 打平
- 结论：崩溃版本数据失真，修复后为最终可信数据

---

## 6. 教训与最佳实践

1. **RAII 语义必须精确**：`unique_ptr` 绝不应当持有"另一个所有者的别名"；若同一对象被多处引用，用 `raw 指针` 或 `shared_ptr` 表达语义，或用 `release()` 显式交接所有权。
2. **崩溃排查要抓确定性**：偏移一致的 segfault = 确定性 bug（如 double free / UAF），优先怀疑生命周期/所有权，而非随机竞态。
3. **本地复现是最高效手段**：线上环境难下 gdb 时，先保证本地二进制与线上 md5 一致 → 本地复现 → gdb 抓栈 → 反汇编定位。
4. **Restart=always 会掩盖崩溃**：压测/稳定性检查必须看 `NRestarts`、`dmesg`、崩溃偏移，不能只看 `is-active`。
5. **压测结论必须保证被测对象健康**：若服务端在压测中反复重启，任何性能数据都不可信——先修崩溃，再谈性能。

---

## 7. 相关文件

| 文件 | 说明 |
|---|---|
| `src/server/vless_connection.cpp` | 修复文件（v0.0.2） |
| `doc/26-benchmark-report.md` | 压测综合报告（含修复后数据） |
| `bench/bench_multi.py` | 压测期间用的自动化脚本 |
| `doc/dev/benchmark/03-second-round.md` | 复测记录（开发期） |
