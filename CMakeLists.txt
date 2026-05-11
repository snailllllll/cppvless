cmake_minimum_required(VERSION 3.20)
project(vmess C CXX)

# 使用 C++20（不需要模块支持）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C 标准（BLAKE3 C 源文件需要）
set(CMAKE_C_STANDARD 11)

# 编译选项
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fcoroutines>)  # GCC 需要显式启用 C++20 协程

# 依赖
find_package(OpenSSL REQUIRED)

# 构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# 子目录
add_subdirectory(src)
add_subdirectory(tests)

# 打印配置信息
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER}")
