# 主程序可执行文件
add_executable(vmess_server main.cpp)

# 链接库
target_link_libraries(vmess_server PRIVATE pthread uring)

# 添加其他源文件后取消注释
# file(GLOB SOURCES "*.cpp")
# add_executable(vmess_server ${SOURCES})
