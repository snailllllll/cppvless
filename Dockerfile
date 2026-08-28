FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    liburing2 \
    && rm -rf /var/lib/apt/lists/*
COPY vless_server /usr/local/bin/vless_server
RUN chmod +x /usr/local/bin/vless_server
# 配置目录：首次启动自动生成 /etc/vless/config.json（含随机 UUID），
# 挂载 volume 可持久化用户配置与连接信息
VOLUME ["/etc/vless"]
EXPOSE 1080/tcp 8848/tcp
ENTRYPOINT ["/usr/local/bin/vless_server"]
CMD ["--config", "/etc/vless/config.json"]
