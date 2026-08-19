FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    liburing2 \
    && rm -rf /var/lib/apt/lists/*
COPY vmess_server /usr/local/bin/vmess_server
RUN chmod +x /usr/local/bin/vmess_server
# 配置目录：首次启动自动生成 /etc/vmess/config.json（含随机 UUID），
# 挂载 volume 可持久化用户配置与连接信息
VOLUME ["/etc/vmess"]
EXPOSE 1080/tcp 8848/tcp
ENTRYPOINT ["/usr/local/bin/vmess_server"]
CMD ["--config", "/etc/vmess/config.json"]
