# 排障记录：double-free 崩溃 + fd 资源问题

> ⚠️ **历史记录说明**：本文为事发时的现场记录，文中 `vmess_server`/`/etc/vmess` 等名称为当时命名；项目现已更名 cppvless，二进制与路径为 `vless_server`/`/etc/vless`，命令请按新名对照执行。

> 状态：**已修复并验证**（v0.0.2）；fd 问题**已分析验证、方案已沉淀、未实施**
> 日期：2026-08-19（double-free）、2026-08-20（fd 分析）
> 关联：`doc/26-benchmark-report.md`（压测综合报告）、`bench/bench_multi.py`
> 摘要：压测期间发现 cpp server 在高频明文建连场景反复 segfault（systemd NRestarts=19），定位为 `VlessConnection` 析构 double-free，修复后 15.7 万连接零崩溃。8 核高并发压测又遇 EMFILE，经实验验证为限额不足而非泄漏；无空闲超时是剩余防御性风险，修复方案见 §8。

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

## 8. fd 资源问题分析与验证（2026-08-20）

> 状态：**已分析验证，修复方案已沉淀，暂未实施**（等待决策）
> 背景：8 核高并发压测时 cpp server 出现 `Accept failed: -24`（EMFILE），需要区分是"fd 泄漏"还是"限额不足"，并对"僵尸连接永久占用 fd"的防御性风险给出方案。
> 方案设计详见 `doc/dev/design/fd-idle-timeout-design.md`（未入库，本地 dev 目录）。

### 8.1 事件回顾：EMFILE 的根因是限额不足，不是泄漏

- **症状**：8 核高并发 HTTP 压测时 cpp p90 骤升到 1.28s，日志刷 `socket() failed` / `Accept failed: -24`（EMFILE），165 万条 ERROR。
- **根因**：systemd 默认 `LimitNOFILE=1024`，8 核高并发（每连接 2-4 fd：clientFd+targetFd+eventfd）瞬间打满。
- **修复**（部署配置，非代码）：`/etc/systemd/system/vmess.service.d/nofile.conf` 设 `LimitNOFILE=65535`。
- **验证**：并发 400 下 cpp p90 从 1.28s → 2.61ms（消除 99.8% 尾部延迟）。

### 8.2 fd 生命周期（正常路径，代码级）

```
accept → clientFd → connections_[fd] = conn
        └─ connectTarget → targetFd
        └─ relay → clientTask 结束 → finishClientTask
        └─ targetTask 结束 → closed_ = true
        └─ cleanupClosedConnections: isClosed() → erase → ~VlessConnection → doClose
        └─ doClose: cancelFd(两个fd) → close(targetFd) → close(clientFd) → 置 -1
```

- `doClose` 幂等（close 后置 -1）
- TCP 半关闭：`shutdown(targetFd_, SHUT_RDWR)` 唤醒对端 recv → EOF → targetTask 结束 → `closed_` 置位

### 8.3 泄漏验证实验（全部在 8 核机上实测）

| 场景 | 方法 | 结果 |
|---|---|---|
| UDP 正常断开 | 200 轮 ASSOCIATE+数据+close | fd 回落基线 36，**无泄漏** |
| UDP 主动关闭 | 200 会话保持后 close | fd 436→36 回落，**无泄漏** |
| UDP 进程崩溃 | 200 子进程 `os._exit`（发 RST） | fd 恒 36，**无泄漏** |
| UDP 半开悬挂 | 50 会话静默保持后关闭 | fd 136→36 回落，**无泄漏** |
| TCP 高频建连 | 300s / 222.8 万连接 | 压测中 fd 平稳（~66），结束后回落 36，**无泄漏** |

**结论**：代码静态分析曾预测"UDP 无 EOF → targetTask 卡死 → fd 泄漏"，被实验证伪——Linux `shutdown()` 会唤醒已 connect UDP socket 的挂起 `recvfrom` 返回 EOF，targetTask 能正常退出。**当前代码不存在 fd 泄漏**。

### 8.4 剩余风险：无空闲超时（僵尸连接永久占用 fd）

风险不在"泄漏"而在"**无 idle timeout**"。当客户端**静默失联**（TCP 连接既不发 FIN 也不发 RST：断网未触发 keepalive、客户端死锁、恶意连接）时：

- 服务端 `co_await stream.read()` 永久挂起
- 连接对象 + 2 个 fd 无限期占用
- 攻击者可建立大量连接后静默保持 → 耗尽 fd → EMFILE 拒绝服务

### 8.5 完整修复方案（按推荐优先级，未实施）

| # | 方案 | 实现 | 风险 | 同类产品对标 |
|---|---|---|---|---|
| **A** | **空闲超时（Idle Timeout）** | 每连接记 `lastActivity`，relay 循环检查；或 `poll(fd, POLLIN, 超时)` 替代永久阻塞 read | 低 | v2ray/Xray：UDP 会话 60s 空闲回收 |
| **B** | **心跳/保活探测** | 周期写探测，超时未响应则关闭 | 中 | Trojan/V2Ray mux：keepalive 区分死连接 |
| **C** | **EventLoop 定期扫描** | 维护 `(fd, lastActivity)` 表，定时扫描超阈值连接强制 `doClose()` | 低 | Nginx `keepalive_timeout`；HAProxy `timeout client/server` |
| **D** | **全局 fd 水位监控** | 定期统计 fd 数，超阈值告警/拒新连接 | 低 | 各反代 `max_conns` 连接数限制 |
| **E** | **io_uring timeout op** | 用 `IORING_OP_TIMEOUT` 在超时后唤醒挂起协程（需扩展 `uring_awaitable.h`） | 中 | io_uring 生态事件循环标准做法 |

### 8.6 同类产品/服务器最常用的做法（业界共识）

1. **v2ray / Xray**（本项目直接对标）：UDP 会话 `connIdle` 60s 无流量销毁；inbound/outbound `ReadTimeout` 默认 60s。= 方案 A + 惰性回收
2. **Nginx**：`keepalive_timeout`（默认 75s）+ `worker_connections` 限制总连接。= 方案 A + D
3. **HAProxy / Envoy**：`timeout client/server`（10s-50s）；Envoy `idle_timeout`（HTTP/2 流 5min）。= 方案 A
4. **ShadowSocks / Trojan**：依赖内核 keepalive + 应用层读超时。= 方案 A

**业界共识**：**方案 A（per-connection idle timeout）最主流、最简单、最低风险**；**方案 C（周期扫描兜底）**作为第二道防线。

### 8.7 推荐实施路径（待决策）

1. **主方案 A**：
   - UDP 会话：`relayUdpClientToTarget` / `relayUdpTargetToClient` 循环里，对 `clientStream.read()` 用 `poll(fd, POLLIN, 60_000)` 包装，60s 无数据则 `doClose()`（对标 v2ray UDP connIdle）
   - TCP 会话：`copyStream` 的 read 加 idle timeout（如 5min，TCP 长连接场景）
2. **兜底 C**：`EventLoop` 维护 `lastActivity` 周期扫描（如 5min），强制回收
3. **进阶 E**：加入 `IORING_OP_TIMEOUT` 支持，内核级定时器唤醒，比 poll 更优雅、与现有架构一致

> 待决策项：idle timeout 默认值（UDP 60s / TCP 5min）、是否先实现方案 A 还是直接做方案 E、客户端 `socks5_connection.cpp` 是否同步处理。

---

## 9. 相关文件

| 文件 | 说明 |
|---|---|
| `src/server/vless_connection.cpp` | 修复文件（v0.0.2） |
| `doc/26-benchmark-report.md` | 压测综合报告（含修复后数据） |
| `bench/bench_multi.py` | 压测期间用的自动化脚本 |
| `bench/bench_udp_leak*.py` | UDP 泄漏验证脚本（8.3 实验用） |
| `bench/bench_vless_udp_raw.py` | 原始 VLESS UDP 压测脚本 |
| `bench/bench_vless_udp_idle.py` | 僵尸会话验证脚本 |
| `bench/bench_vless_udp_crash.py` | 客户端崩溃模拟脚本 |
| `doc/dev/benchmark/03-second-round.md` | 复测记录（开发期） |
