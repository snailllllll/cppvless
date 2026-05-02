# Socket Echo 测试
add_executable(test_socket_echo socket_echo_test.cpp)
target_link_libraries(test_socket_echo PRIVATE socket pthread)

# io_uring Echo 测试
add_executable(test_io_uring_echo io_uring_echo_test.cpp)
target_link_libraries(test_io_uring_echo PRIVATE io_uring socket pthread)

# 添加更多测试
# add_executable(test_io_uring test_io_uring.cpp)
# target_link_libraries(test_io_uring PRIVATE uring)
