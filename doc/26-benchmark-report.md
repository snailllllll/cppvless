# cpp-vless vs Go (Xray/v2ray) 性能压测综合报告

> 状态：**正式发布**（v0.0.2）
> 日期：2026-08-19
> 目标：在同等内网环境下，用固定 Xray 官方客户端分别连接 cpp-vless 与 Go(v2ray) 服务端，对比连接建立速率、转发吞吐与 CPU 效率
> 关联：`doc/27-double-free-troubleshooting.md`（排障记录）、`doc/benchmark-data/`（原始数据）、`doc/dev/benchmark/`（开发期过程文档）、`tencentcloud/*.py`（实例管理）
>
> 本文档为**最终结论**，开发过程中的首轮/复测迭代记录归档于 `doc/dev/benchmark/`，不在此区分轮次。

---

## 1. 结论速览

### 1.1 2 核 2G（SA2.MEDIUM2，首轮基线）

| 指标 | cpp-vless | go (v2ray) | 差距 |
|---|---|---|---|
| **L1 连接建立速率**（明文） | **3,910 conn/s** | **3,912 conn/s** | **打平（0.05%）** |
| **L2 转发吞吐**（明文，iperf3 单流） | **1.580 Gbps** | **1.630 Gbps** | cpp 略低 ~3% |
| **L2 转发吞吐**（并发 2/4/8 流） | 1.46–1.60 Gbps | 1.62–1.66 Gbps | cpp 略低 3–10% |
| **转发 CPU 效率** | **105%**（~0.66 核/Gbps） | **69%**（~0.42 核/Gbps） | cpp 多耗 ~57% CPU |

- **连接建立打平**：修复 cpp server 的 double-free 崩溃后，两者并发建连能力无显著差异
- **吞吐基本持平**：2 核内网下 cpp 与 go 端到端吞吐接近，cpp 略低 ~3%
- **CPU 效率 go 更优**：2 核下 cpp 的 io_uring+协程每 Gbps 多耗约 57% CPU

### 1.2 8 核 16G（SA4.2XLARGE16，高并发复测）

| 指标 | cpp-vless | go (v2ray) | 差距 |
|---|---|---|---|
| **L1 连接建立**（并发 100–1000） | 7.1–7.6k conn/s | 7.1–7.6k conn/s | **打平** |
| **L2 吞吐**（iperf3 -P 64/128） | 10.8–10.9 Gbps | 10.9 Gbps | **打平**（网络上限） |
| **L2 CPU 效率**（-P 64/128） | **390–399%**（~4 核） | **498–501%**（~5 核） | **cpp 省 ~22% CPU** |
| **HTTP 延迟**（并发 1200） | p99 **4.58ms** | p99 **4.75ms** | **打平** |
| **连接内存**（2000 连接保持） | **45 MB** | **84–97 MB** | **cpp 省 ~54%** |

- **高并发下 io_uring 优势显现**：同样顶到 10.9 Gbps 网络上限时，cpp 用 ~4 核 vs go ~5 核（省 ~22% CPU）——与 2 核下"cpp 多耗 CPU"结论**反转**
- **无栈协程内存优势显著**：2000 并发连接下 cpp 常驻内存仅 go 的 ~50%（无栈协程 ~100B 帧 vs goroutine 2KB+ 栈）
- **延迟打平**：修复 EMFILE 后，高并发 HTTP 下两端 p99 均 <5ms

> 核心结论：**cpp 的 io_uring + 无栈协程优势在高并发（数百连接）+ 真实数据面 + 多核机型上才能体现**；低并发/单流场景与 go 打平或略低。所有数据基于 ABAB 交替多轮中位数，原始数据见 `doc/benchmark-data/`。

---

## 2. 测试环境

### 2.1 硬件拓扑（腾讯云南京 CVM 竞价实例，同 VPC 内网）

两套规格独立压测：2 核 2G 为基线，8 核 16G 为高并发复测。拓扑一致：

```
┌─────────────────────────────────┐   ┌─────────────────────────────────┐
│ 压测端  (8c: ins-9od8tycs 10.206.0.10)│   │ 被测端  (8c: ins-pjpl5zpi 10.206.0.4)│
│  - Xray 1.8.24 客户端（3 条链路）  │◄─┘│  - cpp server / v2ray 交替       │
│  - iperf3 client + proxychains   │   │  - iperf3 server :5201           │
│  - bench_multi.py / bench_conn.py│   │  - nginx :80（HTTP 后端目标）     │
└─────────────────────────────────┘   └─────────────────────────────────┘
        内网延迟 < 1ms，无公网抖动
```

> ⚠️ **内网带宽波动**：腾讯云共享内网，实测在 4.3 ~ 11.5 Gbps 间波动（同子网其他租户干扰）。
> L2 吞吐绝对数值受此影响；ABAB 交替 + 同机对比保证 cpp/go 相对公平。

### 2.2 实例规格（腾讯云官方数据 + 实例内实测）

| 规格 | 机型 | vCPU/内存 | CPU 型号 | 主频 | 实例 ID（被测/压测） |
|---|---|---|---|---|---|
| 基线 | **SA2.MEDIUM2**（标准型 SA2） | 2 核 / 2 GB | AMD EPYC 7K62 48-Core | ~2.6 GHz | ins-jbkczrgu / ins-5i4t05a8 |
| 高并发 | **SA4.2XLARGE16**（标准型 SA4） | 8 核 / 16 GB | AMD EPYC 9K84 96-Core | ~2.6 GHz | ins-pjpl5zpi / ins-9od8tycs |

| 公共项 | 值 |
|---|---|
| 镜像 | Ubuntu Server 26.04 LTS 64 位（img-dk53t5vb） |
| 计费 | 竞价 SPOTPAID（按量） |
| 系统盘 | 2核: 20G / 8核: 40G CLOUD_PREMIUM 云盘 |
| 公网带宽 | 200 Mbps 按量（内网不占用） |
| 网络 | 同一 VPC（vpc-egu78q6r），2核在 ap-nanjing-1，8核在 ap-nanjing-3（同 VPC 不同子网，各自互测） |

> 机型信息来自腾讯云 `DescribeInstanceTypeConfigs` API；CPU 型号/主频来自实例内 `lscpu`。

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
- L2：切换 proxychains 指向目标端口后跑 N 轮 iperf3（默认单流；`--parallel N` 用 `-P N` 多流）
- 每轮压测期间后台线程经 SSH 到被测端 `pidstat` 采样被测进程瞬时 CPU
- L2 额外采样**压测端 xray 进程 CPU**，用于判断瓶颈在哪一端
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
# L2：7 轮，每轮 8s（单流）
python3 bench/bench_multi.py --rounds 7 --duration 8 --mode l2 --out /tmp/bench_l2.csv
# L2 并发流对比（iperf3 -P N）
python3 bench/bench_multi.py --rounds 3 --duration 8 --parallel 2 --mode l2 --out /tmp/bench_l2_p2.csv
python3 bench/bench_multi.py --rounds 3 --duration 8 --parallel 4 --mode l2 --out /tmp/bench_l2_p4.csv
python3 bench/bench_multi.py --rounds 3 --duration 8 --parallel 8 --mode l2 --out /tmp/bench_l2_p8.csv
# L1 并发梯度验证平台
python3 bench/bench_multi.py --rounds 3 --conns 100 --duration 6 --mode l1
python3 bench/bench_multi.py --rounds 3 --conns 200 --duration 6 --mode l1
```

**输出**：CSV 列 `mode,side,round,metric,cpu_max_pct`；终端输出每轮明细 + 中位数汇总 + 压测端 xray CPU。

### 4.4 `bench_curl_latency.sh`（HTTP 高并发延迟，curl 并发）

**位置**：压测端 `/opt/bench/bench_curl_latency.sh`

```bash
# 用法: ./bench_curl_latency.sh <socks端口> <名称> <并发> <请求数>
/opt/bench/bench_curl_latency.sh 10883 CPP 400 1600
/opt/bench/bench_curl_latency.sh 10882 GO 400 1600
```

原理：curl 并发（真实客户端行为），`-w %{time_total}` 收集 RTT，awk 输出 p50/p90/p95/p99/max。
**注意**：不用 python `recv` 等 EOF 的脚本——那会引入"等待 FIN"的测量伪影（假 1s 延迟），curl 读 body 即返回。

### 4.5 `bench_keepalive.py`（连接内存观测）

**位置**：压测端 `/opt/bench/bench_keepalive.py`

```bash
# 用法: python3 bench_keepalive.py <socks端口> <名称> <连接数> <保持秒>
python3 /opt/bench/bench_keepalive.py 10883 CPP 2000 18
```

原理：建立并保持 N 条 SOCKS5+VLESS 隧道（目标 nginx:80 不发请求，连接保持），配合被测端
`grep VmRSS /proc/<pid>/status` 观测连接内存。目标不能用 iperf3 server（单连接模式会关闭多余连接）。

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

> 完整逐轮原始数据见 `doc/benchmark-data/`（`bench_l1.csv`、`bench_l2.csv`、`bench_l1_c100.csv`、`bench_l1_c200.csv`、`bench_l2_p1/p2/p4/p8.csv`）。以下为关键数据汇总。

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

### 6.4 L2 并发流对比（iperf3 -P N，3 轮取中位数，8s/轮）

> 验证"增大并发流能否突破单流吞吐上限"，同时采集压测端 xray CPU 判断瓶颈端。

| 并发流 P | cpp_plain (Gbps) | go_plain (Gbps) | cpp CPU | go CPU | 压测端 xray CPU |
|---|---|---|---|---|---|
| 1 | 1.440 | 1.630 | 112% | 64% | 26% |
| 2 | 1.460 | 1.620 | 100% | 57% | 43.5% |
| 4 | 1.600 | 1.630 | 103% | 94% | 39% |
| 8 | 1.490 | 1.660 | 101% | 107% | 38.5% |

- **并发流不提升吞吐**：两端均停留在 ~1.6 Gbps 平台（单流 cpp 1.44 / go 1.63，并发 8 时 cpp 1.49 / go 1.66），瓶颈不在服务端并发能力。
- **压测端 xray CPU 始终 ≤44%**：压测端不是瓶颈（2 核下仍有充足余量）。
- **瓶颈定位**：吞吐平台大概率来自实例网络/链路（同 VPC 内网小实例带宽限制或链路 TCP 特性），与 CPU 无关。
- **结论**：并发流下 cpp 略低 3–10%，差距未随并发扩大；与单流结论一致。

### 6.5 CPU 效率（压测期间瞬时 %CPU 峰值，各轮取中位数）

| 场景 | cpp_plain | go_plain | 每 Gbps CPU |
|---|---|---|---|
| L1（并发 50） | 57% | 44% | — |
| L2（单流吞吐） | **105%** | **69%** | cpp ~0.66 核/Gbps vs go ~0.42 核/Gbps |

- **cpp 每 Gbps 多耗约 57% CPU**——io_uring 两段拷贝 + 每连接独立提交 SQE 的开销。
- 注：并发流场景下 go 的 CPU 随流数上升（P=8 时 107% 打满），cpp 始终 ~100%+；两者吞吐都被网络平台限制，CPU 差距不转化为吞吐差。

---

## 6.6 8 核高并发：L1 连接建立（SA4.2XLARGE16）

| 并发 | cpp_plain (conn/s) | go_plain (conn/s) |
|---|---|---|
| 100 | 7589 | 7598 |
| 200 | 7504 | 7495 |
| 400 | 7327 | 7351 |
| 600 | ~7130 | ~7120 |
| 800 | ~7130 | ~7115 |
| 1000 | ~7110 | ~7080 |

- 3-5 轮中位数。并发 600+ 后速率平稳不再上升——**压测端 Python 线程/GIL 成瓶颈**（被测端 CPU 未打满：cpp 54% / go 38%）。
- **结论：8 核下 cpp 与 go 持续打平**，绝对能力为 2 核的 ~1.8 倍。

## 6.7 8 核高并发：L2 吞吐 + CPU 效率（iperf3 -P N）

> ⚠️ 内网带宽波动（4.3~11.5 Gbps）。以下为高带宽窗口（~10.9 Gbps）数据，ABAB 交替保证相对公平；低带宽窗口两端同样打平（如 P=32 时 4.35 vs 4.36 Gbps）。

| 并发流 P | cpp 吞吐 | go 吞吐 | cpp CPU | go CPU | 压测端 xray CPU |
|---|---|---|---|---|---|
| 8 | 10.8 Gbps | 10.9 Gbps | 334% | 304% | — |
| 16 | 10.8 Gbps | 10.9 Gbps | 370% | 406% | 81% |
| 32 | 10.9 Gbps | 10.9 Gbps | 379% | 468% | 95.5% |
| 64 | **10.90 Gbps** | **10.90 Gbps** | **390%** | **501%** | 107% |
| 128 | **10.80 Gbps** | **10.90 Gbps** | **399%** | **498%** | 116% |

- **吞吐打平**：两端均顶到 ~10.9 Gbps 内网带宽上限（网络是硬瓶颈）。
- **CPU 效率反转（关键发现）**：高并发下 cpp 用 **~4 核** 达成 go **~5 核** 的同样吞吐，**cpp 省 ~22% CPU**——与 2 核下"cpp 多耗 57%"结论相反。
- **机理**：io_uring 批量提交 SQE + 减少系统调用，在连接多、读写频繁时收益放大；goroutine 调度（可抢占、M:N）在低并发时更成熟，高并发下开销上升。
- 注：P=64/128 时压测端 xray CPU 达 107-116%（压测端接近瓶颈），绝对吞吐受此影响但两端一致。

## 6.8 8 核高并发：HTTP 延迟（curl 并发，内网 nginx）

> 用 curl 并发（真实客户端行为，读 body 即返回，不傻等 FIN）。修复 EMFILE 后数据。

| 并发 | cpp p50/p90/p99 | go p50/p90/p99 |
|---|---|---|
| 400 | 1.40 / 2.61 / 6.55 ms | 1.39 / 2.67 / 5.23 ms |
| 800 | 1.30 / 2.64 / 4.74 ms | 1.47 / 3.90 / 7.11 ms |
| 1200 | 1.28 / 2.61 / 4.58 ms | 1.34 / 2.67 / 4.75 ms |

- **延迟打平**：并发 1200 下两端 p99 均 <5ms。
- **修复前对比**：修复 EMFILE 前 cpp 并发 400 时 p90=1.28s（10% 请求被 fd 耗尽阻塞），修复后 p90=2.61ms（消除 99.8% 尾部延迟）。见 §7.2。

## 6.9 8 核高并发：连接内存（2000 连接保持）

| 连接数 | cpp VmRSS | go VmRSS | 差距 |
|---|---|---|---|
| 2000 | **45 MB** | **84–97 MB** | **cpp 省 ~54%** |

- **无栈协程内存优势**：cpp 每连接协程帧 ~100B vs go goroutine 2KB+ 栈（动态增长），高连接数下内存差异显性化。
- 方法：`bench_keepalive.py` 建立并保持 N 条 SOCKS5+VLESS 隧道（目标 nginx:80 不发请求，连接保持），`/proc/<pid>/status` 读 VmRSS。

---

## 7. 发现的严重问题与修复（详见 `doc/27-double-free-troubleshooting.md`）

压测过程中发现 cpp server 在明文高频建连场景下**反复 segfault**（systemd NRestarts 一度达 19），导致服务端周期重启、数据失真。定位为 **`VlessConnection` 析构 double-free**（明文模式 `clientStream_` 与 `rawStream_` 指向同一对象，析构时双重 delete）。已修复并发布 v0.0.2：

- **修复**：`src/server/vless_connection.cpp` 析构函数，明文模式（`tlsCtx_==nullptr`）先 `clientStream_.release()`。
- **验证**：本地 gdb 复现崩溃栈→修复；本地 3 轮共 15.7 万连接零崩溃；压测全程 `NRestarts=0`。
- **影响**：修复后 L1 连接建立速率 cpp 由 ~3,000 提升至 ~3,900 conn/s 并与 go 打平。本报告所有数据均为**修复后**结果。

### 7.2 高并发 EMFILE（fd 耗尽）——8 核复测新发现

- **症状**：8 核高并发 HTTP 压测时 cpp p90 骤升到 1.28s，日志刷 `socket() failed` / `Accept failed: -24`（EMFILE），165 万条 ERROR。
- **根因**：systemd 默认 `LimitNOFILE=1024`，8 核高并发（每连接 2-4 fd：客户端+目标+eventfd）瞬间打满 → accept 失败 + 连接排队。
- **修复**（部署配置，非代码）：`/etc/systemd/system/vmess.service.d/nofile.conf` 设 `LimitNOFILE=65535`，重启后生效。
- **验证**：并发 400 下 cpp p90 从 1.28s → 2.61ms（消除 99.8% 尾部延迟）；fd=36（空闲）正常。
- **教训**：高并发压测前必须确认 `LimitNOFILE`，不能只看 `ulimit -n`（systemd 服务不继承 shell 限制）。

### 7.3 高并发磁盘告警（日志爆炸）

- **症状**：被测端磁盘 100%（系统盘告警），`/var/log/vmess.log` 13G + `syslog.1` 17G。
- **根因**：vmess 以 **debug 级别**运行，高并发建连-断开下每条失败连接都记录 ERROR（8 核下日志量爆炸）。
- **修复**：日志级别降为 `warn`（抑制正常握手失败噪音），truncate 日志，`journalctl --vacuum-size=100M`。
- **验证**：修复后 10s 日志 0 增长，磁盘回落到 16%。

---

## 8. 结论与优化方向

### 8.1 结论

1. **连接建立能力打平**：2 核 ~3,900 conn/s、8 核 ~7.5k conn/s（瓶颈在压测端），cpp 与 go 均打平。
2. **转发吞吐打平（受网络限制）**：2 核 ~1.6 Gbps、8 核 ~10.9 Gbps 内网上限，两端一致。
3. **CPU 效率随场景反转**：
   - 2 核低并发：cpp 每 Gbps 多耗 ~57% CPU（go 优）
   - **8 核高并发（-P 64/128）：cpp 省 ~22% CPU**（cpp ~4 核 vs go ~5 核达同样吞吐）
4. **内存优势（高连接数）**：2000 并发连接下 cpp 常驻内存仅为 go 的 ~54%（无栈协程 vs goroutine 栈）。
5. **HTTP 延迟打平**：并发 1200 下两端 p99 <5ms（需正确配置 `LimitNOFILE`）。

**总体判断**：cpp 的 io_uring + 无栈协程优势在高并发（数百连接）+ 真实数据面 + 多核机型上才充分体现（CPU、内存双优）；低并发/单流场景与 go 打平或略低。若目标场景是"大量并发用户 + 多核"，cpp 架构具备优势；若是低并发单机小流量，两者无显著差异。

### 8.2 优化方向（后续迭代）

1. **压测端升级**（当前最大瓶颈）：Python 线程/GIL + 单 Xray 进程限制并发（L1 600+ 不升、L2 P=128 时压测端 116%），换 Go/多进程客户端或高规格压测端可测出服务端真实上限。
2. **网络隔离**：当前共享内网带宽波动（4.3~11.5 Gbps）干扰 L2 绝对数值，可租用独享带宽/专线或同机多网卡。
3. **TLS vs TLS 公平对比**：给 v2ray 配 TLS（当前 go 只有明文），补全矩阵。
4. **cpp 极致优化**（高并发已优于 go，仍可深化）：provided buffers 零拷贝、批量提交 SQE、per-worker 缓冲池。
5. **运维配置**：部署时必须设置 `LimitNOFILE` 高值 + 日志级别 warn，否则高并发下 EMFILE/磁盘写满。

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
| `doc/benchmark-data/*.csv` | 全部逐轮原始数据（含并发流 bench_l2_p1~p8.csv） |
| `doc/dev/benchmark/01-first-round-method.md` | 开发期：完整压测流程（含从零部署） |
| `doc/dev/benchmark/02-handoff.md` | 开发期：机器接管说明 |
| `doc/dev/benchmark/03-second-round.md` | 开发期：复测与结论修正记录 |
| `bench/bench_multi.py` | 多轮自动化压测脚本（**不入 git**，随开发机保留） |
