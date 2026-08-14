# Socket 封装库
add_library(socket STATIC net/socket.cpp)
target_include_directories(socket PUBLIC ${PROJECT_SOURCE_DIR}/include)

# io_uring 封装库
add_library(io_uring STATIC net/io_uring.cpp)
target_include_directories(io_uring PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(io_uring PUBLIC uring)

# VLESS 协议库
add_library(vless_proxy STATIC
    proxy/vless/decoder.cpp
    proxy/vless/encoder.cpp
    proxy/vless/validator.cpp
    proxy/vless/vision.cpp
    proxy/vless/encryption.cpp
)
target_include_directories(vless_proxy PUBLIC 
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(vless_proxy PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# SOCKS5 协议库
add_library(socks5 STATIC
    proxy/socks5/socks5.cpp
)
target_include_directories(socks5 PUBLIC ${PROJECT_SOURCE_DIR}/include)

# 服务器库
add_library(vmess_server_lib STATIC
    server/event_loop.cpp
    server/vless_connection.cpp
    server/vless_connection_handshake.cpp
    server/vless_connection_connect.cpp
    server/vless_connection_relay_tcp.cpp
    server/vless_connection_relay_udp.cpp
    server/vless_connection_target_task.cpp
)
target_include_directories(vmess_server_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(vmess_server_lib PUBLIC io_uring vless_proxy)

# 客户端库
add_library(vmess_client_lib STATIC
    client/vless_client.cpp
    client/socks5_connection.cpp
    client/socks5_udp_relay.cpp
)
target_include_directories(vmess_client_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(vmess_client_lib PUBLIC io_uring vless_proxy socks5)

# 主程序可执行文件（VLESS 服务端）
add_executable(vmess_server main.cpp)
target_link_libraries(vmess_server PRIVATE
    vmess_server_lib
    pthread
)

# 客户端可执行文件（SOCKS5 → VLESS）
# 复用 EventLoop（vmess_server_lib），静态库只拉取被引用的目标文件
add_executable(vmess_client client/client_main.cpp)
target_link_libraries(vmess_client PRIVATE
    vmess_client_lib
    vmess_server_lib
    pthread
)
