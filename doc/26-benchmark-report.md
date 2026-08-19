# cpp-vless vs Go (Xray/v2ray) 性能压测综合报告

> 状态：**正式发布**（v0.0.2）
> 日期：2026-08-19
> 目标：在同等内网环境下，用固定 Xray 官方客户端分别连接 cpp-vless 与 Go(v2ray) 服务端，对比连接建立速率、转发吞吐与 CPU 效率
> 关联：`doc/27-double-free-troubleshooting.md`（排障记录）、`doc/benchmark-data/`（原始数据）、`doc/dev/benchmark/`（开发期过程文档）、`tencentcloud/*.py`（实例管理）
>
> 本文档为**最终结论**，开发过程中的首轮/复测迭代记录归档于 `doc/dev/benchmark/`，不在此区分轮次。

---

## 1. 结论速览

| 指标 | cpp-vless | go (v2ray) | 差距 |
|---|---|---|---|
| **L1 连接建立速率**（明文） | **3,910 conn/s** | **3,912 conn/s** | **打平（0.05%）** |
| **L2 转发吞吐**（明文，iperf3） | **1.580 Gbps** | **1.630 Gbps** | cpp 略低 ~3% |
| **转发 CPU 效率** | **105%**（~0.66 核/Gbps） | **69%**（~0.42 核/Gbps） | cpp 多耗 ~57% CPU |

- **连接建立打平**：修复 cpp server 的 double-free 崩溃后，两者并发建连能力无显著差异
- **吞吐基本持平**：2 核内网下 cpp 与 go 端到端吞吐接近，cpp 略低 ~3%
- **CPU 效率 go 更优**：cpp 的 io_uring+协程实现每 Gbps 多耗约 57% CPU，是最值得优化的点
- 所有数据基于 7 轮 ABAB 交替压测的中位数，原始数据见 `doc/benchmark-data/`

---

## 2. 测试环境

### 2.1 硬件拓扑（腾讯云南京 CVM 竞价实例，同 VPC 内网）

```
┌─────────────────────────────────┐   ┌─────────────────────────────────┐
│ 压测端  ins-5i4t05a8 (10.206.16.15)│   │ 被测端  ins-jbkczrgu (10.206.16.17)│
│  - Xray 1.8.24 客户端（3 条链路）  │◄─┘│  - cpp server / v2ray 交替       │
│  - iperf3 client + proxychains   │   │  - iperf3 server :5201           │
│  - bench_multi.py / bench_conn.py│   │  - nginx :80（HTTP 后端目标）     │
└─────────────────────────────────┘   └─────────────────────────────────┘
        内网延迟 < 1ms，无公网抖动，带宽充足（不存在带宽墙）
```

### 2.2 实例规格（两台相同，腾讯云官方数据）

| 项 | 值 | 来源 |
|---|---|---|
| 机型 | **SA2.MEDIUM2**（标准型 SA2 家族） | 腾讯云 `DescribeInstanceTypeConfigs` |
| vCPU / 内存 | **2 核 / 2 GB** | 腾讯云 API + 实例内 `lscpu` |
| **CPU 型号** | **AMD EPYC 7K62 48-Core Processor** | 实例内 `lscpu` / `/proc/cpuinfo` |
| **CPU 主频** | **~2.6 GHz**（实测 2595 MHz） | 实例内 `/proc/cpuinfo` |
| 镜像 | Ubuntu Server 26.04 LTS 64 位（img-dk53t5vb） | 腾讯云 API |
| 计费 | 竞价 SPOTPAID（按量） | — |
| 系统盘 | 20G CLOUD_PREMIUM 云盘 | — |
| 带宽 | 200 Mbps 按量（内网不占用） | — |
| 网络 | 同一 VPC + 同一子网（内网互通） | — |

> 注：SA2 标准型为腾讯云 AMD EPYC 7K62 平台，主频 2.6GHz。本报告以实例内实测为准。

### 2.3 被测版本

| 组件 | 版本 | 说明 |
|---|---|---|
| cpp-vless 服务端 | **v0.0.2** | 含 TLS 支持；已修复 double-free 崩溃（见 §6） |
| v2ray | 5.44.1（V2Fly） | VLESS inbound 明文 8444，UUID 与 cpp 一致 |
| Xray 客户端 | 1.8.24（官方 release） | socks inbound + vless outbound |

---

## 3. 压测方案

### 3.1 为什么固定 Xray 客户端

用户场景最有代表性的是"常规客户端连服务端"。cpp 自带客户端依赖 io_uring、场景窄；Xray 官方客户端是 Go 实现、生态最广。因此固定用 **Xray 客户端**分别连 cpp 与 go 服务端，才反映真实使用。

### 3.2 指标定义

| 指标 | 定义 | 方法 |
|---|---|---|
| **L1 连接建立速率** | 每秒成功建立的 SOCKS5+VLESS 隧道数（建连后立即关闭） | `bench_conn.py` 并发 50，目标不可达内网地址 `10.255.0.1:1`，只测隧道建立不含后端干扰 |
| **L2 转发吞吐** | 经 VLESS 隧道的端到端 TCP 吞吐 | iperf3 经 proxychains→Xray SOCKS5→VLESS→被测端 iperf3 server |
| **CPU 效率** | 压测期间被测进程瞬时 CPU（pidstat，非 ps 生命周期均值） | 压测同时经 SSH 在压测端采样 `pidstat -p <pid> 1 N` |

### 3.3 公平性铁律（务必遵守，否则结论失真）

1. **必须同配置对比**（都明文或都 TLS）——cpp TLS vs go 明文是不公平的（TLS 加密吃 CPU）
2. **ABAB 交替**：cpp/go 交替跑多轮，消除时序影响，取中位数
3. **同机交替**：cpp 和 v2ray 在同一台机器上跑（同内核/带宽/CPU）
4. **后端不成为瓶颈**：iperf3 server CPU 极低，nginx 用高并发 worker
5. **多轮取中位数**：每项至少 5-7 轮，报告用中位数而非均值（抗单轮抖动）

---

## 4. 压测工具与脚本（不入 git，文档内详细说明）

压测脚本位于开发机 `bench/` 目录（已加入 `.gitignore`，不随仓库发布），部署时同步到压测端 `/opt/bench/`。共 3 个脚本：

### 4.1 `bench_multi.py`（主脚本，多轮自动化）

**位置**：开发机 `bench/bench_multi.py`（推荐运行位置，经 SSH 控制压测端+被测端），同步副本压测端 `/opt/bench/`

**功能**：
- L1：对 cpp-plain(10883) / go-plain(10882) 端口 ABAB 交替跑 N 轮 `bench_conn.py`
- L2：切换 proxychains 指向目标端口后跑 N 轮 iperf3
- 每轮压测期间后台线程经 SSH 到被测端 `pidstat` 采样被测进程瞬时 CPU
- 汇总取中位数，结果落盘 CSV

**SSH 配置**（脚本内常量）：
- `KEY=/data/workspace/tencentcloud/vless.pem`
- `PRESS_HOST=ubuntu@119.45.95.181`（压测端，10.206.16.15）
- `SUT_HOST=ubuntu@119.45.247.118`（被测端，10.206.16.17）
- `SUT_INNER=10.206.16.17`（内网目标）

**用法**：
```bash
# L1：7 轮，并发 50，每轮 10s，ABAB 交替
python3 bench/bench_multi.py --rounds 7 --conns 50 --duration 10 --mode l1 --out /tmp/bench_l1.csv
# L2：7 轮，每轮 8s
python3 bench/bench_multi.py --rounds 7 --duration 8 --mode l2 --out /tmp/bench_l2.csv
# L1 并发梯度验证平台
python3 bench/bench_multi.py --rounds 3 --conns 100 --duration 6 --mode l1
python3 bench/bench_multi.py --rounds 3 --conns 200 --duration 6 --mode l1
```

**输出**：CSV 列 `mode,side,round,metric,cpu_max_pct`；终端输出每轮明细 + 中位数汇总。

### 4.2 `bench_conn.py`（L1 底层脚本）

**位置**：压测端 `/opt/bench/bench_conn.py`

```bash
# 用法: python3 bench_conn.py <socks5端口> <名称> <并发> <时长>
python3 bench_conn.py 10883 CPP 50 10   # cpp 明文
python3 bench_conn.py 10882 GO 50 10    # go 明文
```

原理：并发线程循环 `SOCKS5 握手 → CONNECT 不可达目标 10.255.0.1:1 → 立即关闭`，统计成功数/失败数/速率。

### 4.3 `bench.py`（HTTP 吞吐/延迟脚本）

**位置**：压测端 `/opt/bench/bench.py`。目标已固定为**内网 nginx**（`10.206.16.17:80`），不走公网。

```bash
python3 bench.py 10883 CPP 30 10
```

---

## 5. 测试步骤（复现）

### 5.0 前置：环境健康检查（每轮压测前必做）

```bash
# 压测端三条链路冒烟（全部应 200）
timeout 5 curl -x socks5h://127.0.0.1:10881 -s -o /dev/null -w 'cpp-tls: %{http_code}\n' http://10.206.16.17/
timeout 5 curl -x socks5h://127.0.0.1:10883 -s -o /dev/null -w 'cpp-plain: %{http_code}\n' http://10.206.16.17/
timeout 5 curl -x socks5h://127.0.0.1:10882 -s -o /dev/null -w 'go-plain: %{http_code}\n' http://10.206.16.17/

# 被测端服务确认
ss -tlnp | grep -E '1080|8848|8444|5201|:80 '   # 全部 LISTEN
systemctl is-active vmess                        # active，NRestarts 应为 0
```

### 5.1 L1 连接建立速率

```bash
# 多轮自动化（推荐，开发机运行）
python3 bench/bench_multi.py --rounds 7 --conns 50 --duration 10 --mode l1 --out /tmp/bench_l1.csv
```

### 5.2 L2 转发吞吐

```bash
# 多轮自动化（推荐，开发机运行）
python3 bench/bench_multi.py --rounds 7 --duration 8 --mode l2 --out /tmp/bench_l2.csv
```

### 5.3 CPU 采集

`bench_multi.py` 已内建：每轮压测同时经 SSH 在被测端执行
`pidstat -p <vmess_pid> -p <v2ray_pid> 1 <duration>`，取各轮瞬时 `%CPU` 峰值的中位数。
（注意：不用 `ps` 的 `%CPU`，那是进程生命周期均值，不反映压测瞬时负载。）

---

## 6. 测试结果（原始数据 + 汇总）

> 完整逐轮原始数据见 `doc/benchmark-data/bench_l1.csv`、`bench_l2.csv`、`bench_l1_c100.csv`、`bench_l1_c200.csv`。以下为关键数据汇总。

### 6.1 L1 连接建立速率（并发 50，10s/轮，7 轮 ABAB 交替）

| 轮次 | cpp_plain (conn/s) | go_plain (conn/s) |
|---|---|---|
| 1 | 4017 | 3802 |
| 2 | 3825 | 3991 |
| 3 | 3781 | 3894 |
| 4 | 3924 | 3858 |
| 5 | 3910 | 3938 |
| 6 | 3794 | 3912 |
| 7 | 4041 | 3945 |
| **中位数** | **3910** | **3912** |

- 所有轮次失败连接数均为 0。
- **结论：连接建立速率打平（差 0.05%）**，无统计意义差异。

### 6.2 L1 并发梯度（3 轮取中位数，6s/轮）

| 并发 | cpp_plain (conn/s) | go_plain (conn/s) |
|---|---|---|
| 50 | 3910 | 3912 |
| 100 | 4048 | 3981 |
| 200 | 3843 | 3812 |

- 并发 100 达平台 ~4000 conn/s；并发 200 略降——压测端 Python 线程/GIL 成瓶颈，非服务端限制。

### 6.3 L2 转发吞吐（8s/轮，7 轮 ABAB 交替）

| 轮次 | cpp_plain (Gbps) | go_plain (Gbps) |
|---|---|---|
| 1 | 1.500 | 1.620 |
| 2 | 1.590 | 1.630 |
| 3 | 1.580 | 1.620 |
| 4 | 1.610 | 1.630 |
| 5 | 1.610 | 1.610 |
| 6 | 1.510 | 1.640 |
| 7 | 1.400 | 1.630 |
| **中位数** | **1.580** | **1.630** |

- retransmits 均为 0。
- **结论：cpp 略低 ~3%，基本持平**。

### 6.4 CPU 效率（压测期间瞬时 %CPU 峰值，各轮取中位数）

| 场景 | cpp_plain | go_plain | 每 Gbps CPU |
|---|---|---|---|
| L1（并发 50） | 57% | 44% | — |
| L2（吞吐） | **105%** | **69%** | cpp ~0.66 核/Gbps vs go ~0.42 核/Gbps |

- **cpp 每 Gbps 多耗约 57% CPU**——io_uring 两段拷贝 + 每连接独立提交 SQE 的开销。

---

## 7. 发现的严重问题与修复（详见 `doc/27-double-free-troubleshooting.md`）

压测过程中发现 cpp server 在明文高频建连场景下**反复 segfault**（systemd NRestarts 一度达 19），导致服务端周期重启、数据失真。定位为 **`VlessConnection` 析构 double-free**（明文模式 `clientStream_` 与 `rawStream_` 指向同一对象，析构时双重 delete）。已修复并发布 v0.0.2：

- **修复**：`src/server/vless_connection.cpp` 析构函数，明文模式（`tlsCtx_==nullptr`）先 `clientStream_.release()`。
- **验证**：本地 gdb 复现崩溃栈→修复；本地 3 轮共 15.7 万连接零崩溃；压测全程 `NRestarts=0`。
- **影响**：修复后 L1 连接建立速率 cpp 由 ~3,000 提升至 ~3,900 conn/s 并与 go 打平。本报告所有数据均为**修复后**结果。

---

## 8. 结论与优化方向

### 8.1 结论

1. **连接建立能力打平**：修复 double-free 后，2 核机型上 cpp 与 go 均可达 ~3,900 conn/s。
2. **转发吞吐基本持平**：cpp 1.58 vs go 1.63 Gbps（差 3%），无显著差距。
3. **CPU 效率是主要差距**：cpp 每 Gbps 多耗 ~57% CPU，是最值得优化的方向。

### 8.2 优化方向（后续迭代）

1. **CPU 效率**（最大差距）：减少内存拷贝（`RecvOp` 的 `buf→data` 两次拷贝 → 固定缓冲池/io_uring provided buffers）；批量提交 SQE（当前每连接独立提交）；减少系统调用次数。
2. **TLS vs TLS 公平对比**：给 v2ray 配 TLS（当前 go 只有明文），补全对比矩阵。
3. **更高并发压测**：压测端 Python 线程在并发 200 时成瓶颈，如需更高并发换 Go/多进程客户端。

---

## 9. 复现环境清单（从零开始）

| 步骤 | 命令/说明 | 文档 |
|---|---|---|
| 1. 创建实例 | `python3 tencentcloud/create_instances.py` + `query_instances.py` | `doc/dev/benchmark/01-first-round-method.md` §3 |
| 2. 部署被测端 | cpp server(v0.0.2) + v2ray + nginx + iperf3，systemd 托管 | 同上 §3.2 |
| 3. 部署压测端 | Xray 三链路 + bench 脚本 + proxychains | 同上 §3.3 |
| 4. 压测 | 见本文档 §5 | 本文档 |
| 5. 测完销毁 | 竞价实例按量计费，`TerminateInstances` | — |

---

## 10. 相关文件

| 文件 | 说明 |
|---|---|
| `doc/26-benchmark-report.md` | 本文档（最终结论） |
| `doc/27-double-free-troubleshooting.md` | double-free 崩溃排障全流程 |
| `doc/benchmark-data/*.csv` | 全部逐轮原始数据 |
| `doc/dev/benchmark/01-first-round-method.md` | 开发期：完整压测流程（含从零部署） |
| `doc/dev/benchmark/02-handoff.md` | 开发期：机器接管说明 |
| `doc/dev/benchmark/03-second-round.md` | 开发期：复测与结论修正记录 |
| `bench/bench_multi.py` | 多轮自动化压测脚本（**不入 git**，随开发机保留） |
