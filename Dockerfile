FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    liburing2 \
    && rm -rf /var/lib/apt/lists/*
COPY vmess_server /usr/local/bin/vmess_server
RUN chmod +x /usr/local/bin/vmess_server
ENTRYPOINT ["/usr/local/bin/vmess_server"]
CMD ["1080", "warn"]
