# 服务端运维调优：TCP BBR 加速跨境链路

> 状态：**已实施并验证**
> 日期：2026-08-15
> 目标环境：腾讯云香港（ap-hongkong）TencentOS Server 4，内核 6.6.117-45.11.3.tl4.x86_64
> 关联：`doc/README.md` 索引、部署脚本 `build.sh` / `deploy.sh` / `fetch_logs.sh`

## 1. 背景

部署在香港节点的 VLESS 服务，实测出现「家用电信宽带慢、手机联通数据快」的显著差异。排查结论：

- **节点出口不慢**：服务器本地 Ookla speedtest 实测下行 537 Mbps / 上行 191 Mbps / 延迟 1.09 ms / 丢包 0%。
- 瓶颈在「客户端 → 香港」的跨境链路，而非节点本身；电信默认 163 骨干网跨境出口拥塞，联通国际出口质量更好。
- 服务器默认拥塞控制是 `cubic`，在跨境高丢包链路上吞吐会因丢包急剧下降，属于可优化的放大因素。

## 2. 原理：为什么 BBR 对跨境链路有效

| | cubic（默认） | BBR |
|---|---|---|
| 拥塞信号 | 以**丢包**为信号，一旦丢包就大幅降窗 | 基于**瓶颈带宽 + RTT** 建模，不把丢包当拥塞 |
| 跨境高丢包场景 | 频繁降速，吞吐塌陷 | 维持较高吞吐 |
| 配合 qdisc | 传统 fq_codel | 官方推荐 `fq` |

跨境线路（尤其晚高峰电信）天然丢包率高，`cubic` 会不断降窗，BBR 则能顶住丢包维持吞吐。**BBR 只影响 TCP，对 UDP 无效**；对新建 TCP 连接即时生效，无需重启应用。

## 3. 检查当前状态

```bash
sysctl net.ipv4.tcp_congestion_control            # cubic = 未开 BBR
sysctl net.core.default_qdisc                     # fq_codel（BBR 建议 fq）
sysctl net.ipv4.tcp_available_congestion_control  # 看 bbr 是否可用
modinfo tcp_bbr | head -3                          # 看模块是否存在
```

## 4. 开启步骤

### 4.1 临时生效（立即生效，重启失效）

```bash
modprobe tcp_bbr                                        # 加载 BBR 内核模块
sysctl -w net.ipv4.tcp_congestion_control=bbr           # 切换到 BBR
sysctl -w net.core.default_qdisc=fq                     # qdisc 改为 fq
```

### 4.2 持久化（重启仍生效）

```bash
printf 'net.ipv4.tcp_congestion_control=bbr\nnet.core.default_qdisc=fq\n' \
  > /etc/sysctl.d/99-bbr.conf
```

### 4.3 验证

```bash
sysctl net.ipv4.tcp_congestion_control   # 应输出 bbr
sysctl net.core.default_qdisc            # 应输出 fq
```

## 5. 回退方法

若发现 BBR 反而更慢（极少见，通常因 qdisc 不匹配或中间设备兼容问题），改回默认：

```bash
sysctl -w net.ipv4.tcp_congestion_control=cubic
sysctl -w net.core.default_qdisc=fq_codel
rm -f /etc/sysctl.d/99-bbr.conf
```

## 6. 配套测速方法

定位「线路瓶颈」还是「代理软件瓶颈」，需分层测速：

1. **节点出口**（服务器自身，已装 Ookla speedtest）：
   ```bash
   /usr/local/bin/speedtest --accept-license --accept-gdpr
   ```
2. **端到端裸线路**（客户端 → 服务器，不经过 VLESS，服务器 iperf3 监听 5201）：
   ```bash
   iperf3 -c <服务器IP> -p 5201        # 测上行
   iperf3 -c <服务器IP> -p 5201 -R     # 测下行（反向）
   iperf3 -c <服务器IP> -p 5201 -R -P 4  # 多并行流
   ```
3. **走代理真实体验**（客户端挂 vmess_client SOCKS5 → fast.com 单连接测速）。

判断：直连慢 → 换线路/运营商出口；直连快但走代理慢 → 查代理软件（io_uring/转发逻辑）。
