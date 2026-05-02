# Socket 封装库
add_library(socket STATIC net/socket.cpp)
target_include_directories(socket PUBLIC ${PROJECT_SOURCE_DIR}/include)

# io_uring 封装库
add_library(io_uring STATIC net/io_uring.cpp)
target_include_directories(io_uring PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(io_uring PUBLIC uring)

# VLESS 协议库
add_library(vless_proxy STATIC proxy/vless/decoder.cpp)
target_include_directories(vless_proxy PUBLIC ${PROJECT_SOURCE_DIR}/include)

# 服务器库
add_library(vmess_server_lib STATIC
    server/event_loop.cpp
    server/vless_connection.cpp
)
target_include_directories(vmess_server_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(vmess_server_lib PUBLIC io_uring vless_proxy)

# 主程序可执行文件
add_executable(vmess_server main.cpp)
target_link_options(vmess_server PRIVATE -static-libgcc -static-libstdc++)
target_link_libraries(vmess_server PRIVATE
    vmess_server_lib
    pthread
)
