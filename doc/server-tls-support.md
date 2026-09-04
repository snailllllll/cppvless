# 服务端内置 TLS 支持设计文档

> 状态：**已实现（阶段 1–3 完成并验证）**；阶段 4（客户端 TLS）待做
> 日期：2026-08-15（设计）/ 2026-08-15（实现）
> 目标版本：v1（VLESS over TLS，服务端内置 TLS 终结）
> 关联：实现顺序参见 `doc/README.md` 索引与 `current-architecture.md`

## 1. 背景

当前生产部署中，iOS 客户端访问被墙站点（twitter/维基百科等）失败，根因是：

- 服务器与客户端之间是**明文 VLESS**，GFW 能识别 VLESS 特征及内层明文 TLS ClientHello 的 SNI（被墙域名），在客户端→服务器方向注入 RST；
- 通过 openresty 8443 做 TLS 终结后问题消失（GFW 只看到 `SNI=9zh.fun` 的加密流量）。

当前依赖 **openresty 反向代理 + 手动证书配置** 才能提供 TLS 终结，运维成本高、配置分散（nginx.conf 手改、升级易丢）。

**目标**：把 TLS 终结能力内置进 `vless_server`，iOS 客户端直接 `VLESS+TLS+SNI` 连接服务器，不再依赖 openresty 与手动反代配置。

## 2. 目标与非目标

### 目标
- 服务端支持监听一个 TLS 端口（默认 443，可配置为任意端口），自行终结 TLS；
- TLS 握手、数据读写全部异步（io_uring + 协程），不阻塞事件循环；
- 协议层（VLESS 握手/中继）完全复用，不因 TLS 写第二套逻辑；
- 证书通过 CLI 参数/环境变量指定本地文件路径，无需外部反代；
- 明文 VLESS 端口保留，与 TLS 端口并存，平滑迁移。

### 非目标（v1）
- ACME 自动申请/续期证书（v2 可考虑，v1 由 1Panel/certbot 签发后指定路径即可）；
- Fallback 伪装（非 VLESS 流量转发到网站伪装成 HTTPS，v2 可选）；
- mTLS / 双向认证。

## 3. 端口选择

**结论：端口可配置，不强制 443。**

| 选项 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| 443 | 最不显眼；防火墙放行概率最高 | 当前服务器 443 已被 openresty 占用 | 作为默认值，仅在端口空闲时使用 |
| 高位端口（8443/2053/2096） | 避免冲突；GFW 主要按 SNI 检测，端口影响小 | 个别网络可能封非标准端口 | 当前部署环境采用 |

**安全性来源是"SNI 为正常域名 + 加密流量"，而非端口号。** 因此端口作为配置项，默认 443，部署时按环境选择。

## 4. 总体架构

```
iOS ──VLESS+TLS──> vless_server:TLS端口（内置 TLS 终结）──> 目标服务器
        │
        └─ 明文 VLESS 端口（保留，兼容现有 openresty 方案与调试）
```

核心抽象：**Stream 接口**（对齐 Go 的 `net.Conn`）：

```
Stream
 ├── AsyncStream（raw fd，现有实现改造为符合接口）
 └── TlsStream（OpenSSL 非阻塞 + io_uring 事件驱动，新增）
```

协议层（Decoder/Encoder/relay）只依赖 Stream 接口，TLS 仅是换一个 Stream 实现。

## 5. 改动清单

| 文件 | 改动 |
|---|---|
| 新增 `include/net/stream.h` | Stream 抽象接口：`read() / writeFull() / shutdownWrite() / fd()` |
| 新增 `include/net/tls_stream.h` + `src/net/tls_stream.cpp` | TlsStream：OpenSSL 非阻塞 + io_uring，处理 WANT_READ/WANT_WRITE |
| `include/coro/async_stream.h` | AsyncStream 实现 Stream 接口 |
| `include/coro/buffered_stream.h` | UringBufferedStream 改为基于 Stream 接口取数 |
| `include/proxy/vless/decoder.h` + `src/proxy/vless/decoder.cpp` | decode() 参数泛化为抽象流 |
| `include/proxy/vless/encoder.h` + `src/proxy/vless/encoder.cpp` | decodeResponse() 参数泛化为抽象流 |
| `include/server/vless_connection.h` + src/ | 成员 stream_ 改为抽象流；中继读写走抽象接口 |
| `include/server/event_loop.h` + `src/server/event_loop.cpp` | 支持 TLS 监听：accept 后异步 TLS 握手再建连接 |
| `src/main.cpp` | 新增 `--tls-port / --cert / --key` 参数；加载 SSL_CTX；为 TLS 端口起 EventLoop |

## 6. 关键技术设计

### 6.1 TlsStream 读写（核心难点）

OpenSSL 非阻塞模式下，SSL_read/SSL_write 返回 ≤0 时用 `SSL_get_error` 判断：

```cpp
// 读
int n = SSL_read(ssl_, buf, size);
if (n > 0) return 数据;
int err = SSL_get_error(ssl_, n);
if (err == SSL_ERROR_WANT_READ) {
    co_await AsyncRecv(fd_, uring_);   // 挂起等 io_uring 可读事件，醒来重试
} else if (err == SSL_ERROR_WANT_WRITE) {
    co_await 等可写;                    // TLS 重协商场景
} else if (err == SSL_ERROR_ZERO_RETURN) {
    return EOF;
} else {
    return 错误;
}
```

### 6.2 TLS 握手协程

accept 得到 fd 后：

```
co_await tlsHandshake(fd, sslCtx)
  ├─ SSL_accept 返回 1 → 完成
  ├─ WANT_READ → co_await AsyncRecv → 重试
  └─ WANT_WRITE → co_await 等可写 → 重试
```

握手完成后将 TlsStream 交给 VlessConnection，协议流程不变。

### 6.3 证书

- 通过 `--cert`/`--key` 指定本地文件路径（复用 9zh.fun 证书或任意 Let's Encrypt 证书）；
- `SSL_CTX_use_certificate_chain_file` + `SSL_CTX_use_PrivateKey_file`，支持 RSA/ECDSA；
- 协议版本 TLSv1.2/1.3。

### 6.3.1 自签证书保底（零配置兜底，**已实现**）

**动机**：D6 决策下"无域名直接用自签证书"仍依赖部署层执行 openssl 命令并挂载进容器；
一旦忘记生成或文件丢失，TLS 端口直接不可用。保底 = 把"生成自签证书"内置进服务器，
让 `--tls-port` 在零外部步骤下也能起来。

**触发条件（语义分级，fail-fast 与保底分开）**：

| 场景 | 行为 |
|---|---|
| `--tls-port` 指定，`--cert`/`--key` 均未指定 | 自签保底（预期行为，warning 日志） |
| `--cert`/`--key` 指定且加载成功 | 使用正式证书，保底不触发 |
| `--cert`/`--key` 指定但文件缺失/加载失败 | **报错退出**（配置错误 fail-fast，不静默降级，避免客户端 allowInsecure 状态下连上自签却不自知） |

**生成实现**（纯 OpenSSL API，不调用外部 openssl 命令，不依赖部署脚本）：

- **ECDSA P-256** + X509 v3（跟随 Xray 官方 `cert.Generate` 选型：签发性能优于 RSA-2048，
  自签保底场景密钥强度非瓶颈）；
- 有效期默认 **1 年**（`--cert-days` 可配；对照 Xray CLI 默认 90 天——我们落盘复用 +
  启动期检测续签，1 年减少运维打扰）；
- **启动期到期检测**：加载到存量自签证书时检查剩余有效期，**剩余 < 30 天则自动重新生成**
  （程序内检测，无需定时任务；避免自签静默过期导致 TLS 端口失效）；
- 落盘到 `--cert-dir`（默认 `./certs`，容器内 `/etc/vless/certs`），文件已存在则复用，
  证书跨重启稳定（已实现，见 `src/net/tls.cpp`）；
- 启动日志打印 warning「自签证书保底生效」。

**实现与设计差异（如实记录）**：

| 设计项 | 设计值 | 实现值 | 影响 |
|---|---|---|---|
| CN | 本机 IP（`getifaddrs` 探测） | 固定 `vless-self-signed` | 不影响 TLS 功能；客户端本就不校验 CN |
| SAN | `getifaddrs` 探测填充 `IP:127.0.0.1, IP:<本机IP>` | 未填充 SAN | 仅影响"显式 IP 校验"的客户端；iOS allowInsecure 场景无感知 |
| 日志指纹 | 打印证书 SHA-256 指纹 | 未打印 | 排障小工具，可后续补充 |

> 结论：实现已满足"零外部步骤 TLS 可用"的核心目标。CN/SAN 精细化（域名场景）
> 与指纹打印列为后续增强，不阻塞 v1。

**安全边界**：自签证书无 CA 背书，仅适合 IP + 个人自用/临时验证；存在正式证书
（`--cert`/`--key`）时保底不触发。客户端（iOS）必须开启 allowInsecure（与 D6 一致）。
生产/公开节点不得依赖保底。

### 6.4 ALPN

固定协商 `h2, http/1.1`（iOS 客户端兼容；VLESS 内层协议不受 ALPN 影响）。

## 7. 配置设计

```
vless_server [--tls-port <port> --cert <path> --key <path> --cert-dir <dir> --cert-days <days>]
             [port] [loglevel] [workers]
```

- 未指定 `--tls-port` → 只起明文端口（保持旧行为）；
- 指定 `--tls-port` 且带 `--cert`/`--key` → 明文 + TLS 双端口，使用正式证书；
- 指定 `--tls-port` 但未指定 `--cert`/`--key` → 明文 + TLS 双端口，自签证书保底（见 6.3.1，warning 日志）；
  指定了但加载失败 → 报错退出（fail-fast，不降级）；
- `--cert-dir`：自签证书落盘目录（默认 `./certs`），仅保底场景使用；
- `--cert-days`：自签证书有效期天数（默认 365），仅保底场景使用；
- 端口默认 443，可配置任意高位端口。

## 8. 兼容性与迁移

- 明文 1080 保留，现有 openresty 8443 方案可继续用，也可切换为直连 TLS 端口；
- iOS 客户端从 `VLESS+TLS+8443(openresty)` 平滑切换到 `VLESS+TLS+服务器TLS端口`，SNI 不变（仍是 9zh.fun）；
- 服务端行为对明文客户端完全不变（回归验证点）。

## 9. 分阶段实施计划

| 阶段 | 内容 | 验证 | 状态 |
|---|---|---|---|
| 1 | Stream 抽象 + 协议层泛化（纯重构，明文行为不变） | CI 构建 + 明文链路回归 | ✅ 完成（明文并发 10/10 + 顺序 20/20） |
| 2 | TlsStream + EventLoop TLS 端口 + 自签证书保底（6.3.1，OpenSSL API 生成） | 服务器本机 `openssl s_client` + 明文/ TLS 双链路 + 缺证书场景起服务 | ✅ 完成（见 §10 验证记录） |
| 3 | CLI 配置 + 部署 | iOS 客户端 VLESS+TLS 实测被墙站点 | ✅ 代码完成（`--tls-port/--cert/--key/--cert-dir/--cert-days`）；iOS 真机待实测 |
| 4 | 客户端（vless_client）TLS：共用 TlsStream | iOS 客户端直连服务器 TLS 端口 | ⏳ 待做（D5） |

## 10. 验证方案

1. 服务器本机：`openssl s_client -connect 127.0.0.1:<tlsport> -servername <域名> -tls1_3`，`Verify return code: 0`；
2. 明文端口回归：现有测试客户端连接 1080 正常；
3. iOS 客户端：`VLESS + TLS + SNI` 直连服务器 TLS 端口，访问 twitter/维基百科；
4. 对比实验：确认不再出现 `-104 RST`（对照本文档第 1 节背景）。

**已执行验证记录（2026-08-15）**：

| 验证项 | 命令/场景 | 结果 |
|---|---|---|
| TLS 握手 + 自签证书 | `echo \| openssl s_client -connect 127.0.0.1:9443` | PASS（TLSv1.3，subject=vless-self-signed） |
| VLESS+TLS 端到端 | `tests/vless_tls_test.py`（自签，CERT_NONE） | PASS（TLSv1.3，213B HTTP 响应） |
| 明文回归（TLS 共存） | `tests/vless_http_test.py` 1080 → 18080 | PASS（5/5 并发） |
| 正式证书路径 | `--cert/--key`（openssl 生成 rsa:2048） | PASS（subject=test.example.com） |
| fail-fast | `--cert /tmp/nonexistent.pem --key ...` | PASS（exit=1，报错退出不降级） |
| 证书复用 | 重启后 md5 一致 + 日志 "reusing existing cert" | PASS |
| 从零完整构建 | `rm -rf build && cmake -B build && cmake --build` | PASS（零错误零警告） |

## 11. 风险与回退

- **协议层泛化回归风险**：明文路径行为必须不变。缓解：阶段 1 独立提交，CI 构建 + 明文回归后再进入阶段 2；
- **TlsStream 非阻塞复杂度**：WANT_READ/WANT_WRITE 处理需严格测试（大数据、半关闭、重协商）；
- **回退**：保留 openresty 8443 方案直到 TLS 直连稳定，随时可切回；
- 服务器升级/重建不涉及 openresty 配置，不再有"配置丢失"风险。

## 12. 决策记录

- [x] D1 端口：默认 443，允许 `--tls-port` 选择任意端口
- [x] D2 证书：服务器只负责读本地文件路径（--cert/--key），证书来源由部署层决定
      （当前单机：1Panel/certbot 签发；未来 K8s：cert-manager 签发后挂载为文件，两者解耦在"路径"）
- [x] D6 **节点以 IP + 自签证书为默认部署目标**（域名不引入项目代码）
      - 项目不硬编码任何域名；服务器只读证书文件，不校验 SNI/不解析自身域名
      - release 的容器部署方案内置"生成自签证书"步骤（openssl 生成 + 挂载进容器），
        无域名即可直接部署
      - 有域名的用户可自行替换为正式证书（Let's Encrypt），更安全/SNI 伪装更好；
        iOS 客户端使用自签证书时需开启 allowInsecure（跳过证书校验）
- [x] D7 **自签证书保底（零配置兜底，v1 定稿）**：`--tls-port` 指定但未传证书时，
      服务器内置生成自签证书（OpenSSL API，ECDSA P-256 / 有效期 1 年 `--cert-days` 可配，
      落盘 `--cert-dir` 复用 + 启动期剩余 < 30 天自动重签），保证 TLS 端口无外部步骤即可起；
      存在正式证书（`--cert`/`--key`）时保底不触发，正式证书加载失败报错退出（不静默降级）；
      仅作兜底，客户端需 allowInsecure
      - 参考：Xray 官方 `common/protocol/tls/cert/cert.go`（ECDSA P-256 选型）与
        `main/commands/all/tls/cert.go`（`xray tls cert` 静态生成，默认 90 天）；
        我们以落盘复用 + 启动检测续签换取更长有效期与更少运维打扰
- [x] D3 明文端口 1080：保留并存；明文继续走现有 openresty 8443 反向代理，内置 TLS 端口为新增直连通道
- [x] D4 fallback 伪装：v1 不做
- [x] D5 客户端（vless_client）TLS：作为阶段 4（服务器 TLS 稳定后），共用 TlsStream

补充决策：
- 证书传参：CLI 参数 + 环境变量（VLESS_CERT/VLESS_KEY）都支持（参数优先）
- TLS 端口与明文端口：共用 worker 池（同进程多端口 accept，worker 复用）
- 预留证书热加载（SIGHUP 重载），v1 可不实现

## 13. 底层组件继承说明

Stream 抽象是**在现有层之上加接口**，底层全部保留复用，不做重写。

```
协议层：Decoder / Encoder / VlessConnection
        （只改：依赖具体类 → 依赖 Stream 接口）
中间层：Stream 接口（新增，~30 行）
        ├─ RawStream（= 现有 AsyncStream，声明改一下）
        └─ TlsStream（新增，内部调用底层原语）
底层：AsyncRecv/Send/Connect/Accept/Shutdown/RecvFrom/SendTo（io_uring 原语）
      CoroutineRegistry / Task / UringBufferedStream（协程基础）
事件循环：EventLoop（保留，仅 accept 后加 TLS 握手分支）
```

| 现有组件 | 命运 |
|---|---|
| `net/io_uring.h/.cpp` | 原封不动 |
| `coro/uring_awaitable.h`（全部 Async* 原语） | 原封不动，TlsStream 靠它们实现 WANT_READ/WANT_WRITE 挂起恢复 |
| `coro/task.h`、`UringOp` + `PendingUringOps` | 原封不动 |
| `coro/buffered_stream.h` | 实现保留，声明为基于 Stream 接口取数 |
| `coro/async_stream.h` | 实现保留，声明为实现 Stream 接口 |
| `server/event_loop.cpp` | 保留，accept 后加"是否走 TLS 握手"分支 |
| `proxy/vless/*`、`VlessConnection` | 类型参数机械替换，协议逻辑零改动 |

典型示例（TlsStream 读）：

```cpp
Task<RecvResult> read() {
    for (;;) {
        int n = SSL_read(ssl_, buf, size);
        if (n > 0) co_return 明文;
        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            co_await AsyncRecv(fd_, uring_);   // 复用现有原语
            continue;
        }
        ...
    }
}
```

## 14. 域名 vs IP 的取舍（讨论记录）

**结论：TLS 不强制需要域名，但强烈推荐有域名。** 服务器代码层面域名非必选（只读证书文件，不校验 SNI、不解析自身域名）。

| 层面 | 有域名 | 只有 IP |
|---|---|---|
| 证书获取 | Let's Encrypt 免费（必须域名验证） | 自签证书 或 付费 IP 证书（贵、CA 支持少） |
| GFW 伪装（SNI） | SNI=正常域名，放行 | SNI=IP/空，纯 IP TLS 流量特征可疑，被识别风险高 |
| 动态节点（竞价实例） | 必须域名（IP 会变，DNS 统一管理） | IP 一变客户端需改配置 |

取舍建议：
- 临时验证/个人自用：IP + 自签证书 + iOS allowInsecure —— **v1 自签保底（6.3.1）内置后，此路径零部署步骤**（无需手动跑 openssl、无需挂载证书）
- 正式/稳定：域名 + Let's Encrypt（免费、零客户端配置；`--cert`/`--key` 指定后保底自动失效）
- 动态节点：必须域名

## 15. 发布流程展望（Draft，不动现有 CI）

两个阶段目标解决后，发布分为两条路径并存：
1. GitHub Releases：打 tag（vX.Y.Z）触发构建，产物（二进制 + 校验和 + 变更说明）上传 Releases，作为受控正式版本入口；
2. 现有 CICD（deploy.yml）：继续跟随 main 自动部署到当前环境（日常迭代）。

Release 产物可手动部署到任意机器（不依赖 CI 部署服务器）。

## 16. 阶段目标总览

- 目标 1：TLS 支持（阶段1 Stream 抽象 → 阶段2 TlsStream → 阶段3 CLI+部署）—— **服务端部分完成**，阶段 4（客户端 TLS）待做
- 目标 2：日志性能改造（异步日志，解耦 worker 与日志 I/O，增强排障信息）—— 方案见 `doc/dev/design/logging-plan.md`，选型讨论中
- 展望：订阅与动态节点自动化、GitHub Releases 发布流程

## 17. 后续展望：订阅与动态节点自动化（Draft）

> 目标形态：标准机场模式——控制面生成订阅，客户端（Shadowrocket/Stash/Clash）定时拉取；
> 节点通过腾讯云竞价实例动态申请/销毁，DNS 统一由控制面管理。
> 该小节为展望，与 v1 TLS 实现解耦，不阻塞主流程。

### 13.1 订阅格式

VLESS 分享链接（客户端可直接识别）：

```
vless://<uuid>@<域名>:<端口>?type=tcp&security=tls&sni=<SNI域名>&fp=chrome#节点名
```

示例：
```
vless://e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b@<域名>:443?type=tcp&security=tls&sni=9zh.fun&fp=chrome#cn-hk-spot-01
```

- 控制面把 `vless://` 列表 base64 编码后通过 HTTP 接口返回（订阅 URL）；
- Clash 系（Stash）可用 YAML proxies 列表，两种格式控制面都生成。

### 13.2 动态节点全流程与耗时

| 步骤 | 耗时（镜像预构建后） |
|---|---|
| API 创建竞价实例 | 30–60s |
| 系统初始化 + cloud-init（装 docker、拉镜像、起服务） | 30–60s |
| 健康检查 | 10–30s |
| DNSPod 改 A 记录（TTL=60s） | API 秒级 + 生效 ≤60s |
| 控制面同步 + 订阅更新 | 秒级 |
| **总计** | **最快 2–3 min，一般 3–5 min** |

关键优化：镜像预构建（省去编译）；DNSPod TTL=60s；cloud-init 串行完成部署与上报。

### 13.3 销毁与注意事项

- 竞价实例按实际使用时长计费，销毁即停止计费（秒/小时级）；
- **IP 会变**：销毁后公网 IP 释放 → 订阅必须用域名而非 IP（DNS 由控制面统一管理）；
- 销毁前：改 DNS 指回其他节点/删除记录 + 控制面移除节点 + 客户端刷新订阅；
- 竞价实例可能被回收（出价/资源紧张）→ 控制面需节点健康监控 + 自动摘除失效节点。

### 13.4 SNI 说明（供文档上下文）

SNI（Server Name Indication）：TLS 握手 ClientHello 中的明文字段，客户端告知服务器要访问的域名，服务器据此选择证书。因明文可见，GFW 可据此识别被墙域名并注入 RST；VLESS+TLS+SNI=正常域名 即为规避手段。
