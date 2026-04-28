# 测试程序
add_executable(test_coroutine test_coroutine.cpp)
target_link_libraries(test_coroutine PRIVATE pthread)

# 添加更多测试
# add_executable(test_io_uring test_io_uring.cpp)
# target_link_libraries(test_io_uring PRIVATE uring)
