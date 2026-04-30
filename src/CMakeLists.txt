# 主程序可执行文件
add_executable(vmess_server main.cpp)

# 链接库
target_link_libraries(vmess_server PRIVATE pthread uring)

# Socket 封装库
add_library(socket STATIC net/socket.cpp)
target_include_directories(socket PUBLIC ${PROJECT_SOURCE_DIR}/include)

# io_uring 封装库
add_library(io_uring STATIC net/io_uring.cpp)
target_include_directories(io_uring PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(io_uring PUBLIC uring)

# 添加其他源文件后取消注释
# file(GLOB SOURCES "*.cpp")
# add_executable(vmess_server ${SOURCES})
