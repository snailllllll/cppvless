# 示例程序

# io_uring Echo 服务器示例
add_executable(uring_echo_server uring_echo_server.cpp)
target_link_libraries(uring_echo_server PRIVATE io_uring socket pthread uring)

# 打印示例程序信息
message(STATUS "Examples: uring_echo_server")
