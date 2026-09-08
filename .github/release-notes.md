> 本文件随版本更新：发版前重写内容，作为该 Release 的说明正文（见 `.github/workflows/release.yml` 的 `body_path`）。
> 只写对外有意义的变化，**文档类提交不写入**。

## v0.0.6

### 性能

- **accept 唤醒后 `accept4` 非阻塞排空 listen backlog**
  原先内核中始终只有 1 条 outstanding accept，突发到达的 K 个连接要经 K 轮事件循环
  才能消费完（backlog 排队 → SYN 重传 → 建连尾延迟）。现在 oneshot accept 仅作唤醒
  信号，唤醒后用非阻塞 `accept4` 一次抽干 backlog，再回到协程挂起：K 个连接 1 轮
  全量就绪。

  实测（8C16G 压测端 → 2C2G 服务端，Go 压测客户端）：

  | 场景 | cppvless | v2ray 5.x（对照） |
  |---|---|---|
  | 2000 并发突发建连 | 2000/2000 成功，p99 **61.6ms**，max 72.4ms | 2000/2000，p99 73.1ms，max 79.3ms |
  | 阶梯加压峰值吞吐 | **27,124 conn/s**（@400 并发） | 26,487 conn/s（@100 并发） |
  | ListenOverflows / ListenDrops | 0 | 0 |

  排空模式同时保留了内核 backlog 作为背压缓冲（loop 忙时不在应用侧堆积 fd）。

### 功能

- **自定义节点名**：`config.json` 新增 `remark` 字段（默认 `cppvless`，旧配置自动兼容），
  分享链接的 `#fragment` 使用该名字；名字按 RFC 3986 百分号编码，支持中文与空格。
  `install.sh` 支持 `VLESS_NODE_NAME` 环境变量，便于批量创建节点时写入
  「地区-带宽」这类自描述名称。

### 变更

- 启动提示：TLS 开启时明确双监听线程数，显示为
  `Workers: N per port group (2N threads total: plaintext + TLS)`。

### 修复

- 测试后端 `HTTPServer` 的 listen backlog 由默认 5 调到 1024，消除并发测试中
  SYN 静默丢弃导致的 1s 重传伪影（仅影响测试）。
- 移除 v0.0.5 引入的 `probe_entry_latency` 探测工具。

### 产物

| 文件 | 说明 |
|---|---|
| `cppvless-linux-<arch>.tar.gz` | `vless_server` + `vless_client` 打包（install.sh 使用，保持兼容） |
| `cppvless-server-linux-<arch>` | **新增**：`vless_server` 裸二进制（无 tar 的环境直接下载使用） |
| `cppvless-client-linux-<arch>` | **新增**：`vless_client` 裸二进制（同上） |
| `*.sha256` | 每个产物各自的 SHA256 校验 |

`<arch>` = `amd64` | `arm64`。产物为容器内静态编译，仅依赖 glibc + OpenSSL3。

### 升级

```bash
# 一键安装/升级（默认 VLESS_SOURCE=binary，从本 Release 取 amd64/arm64 产物）
curl -fsSL https://raw.githubusercontent.com/snailllllll/cppvless/main/install.sh | sudo bash
```

配置与协议无变化，直接替换二进制即可；旧 `config.json` 无需修改（`remark` 可缺省）。
