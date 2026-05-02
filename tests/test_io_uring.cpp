// 测试1: io_uring 异步IO基础
// 编译: g++ -std=c++20 -o test_io_uring test_io_uring.cpp -luring
// 
// 讲解：
// io_uring是Linux 5.1+的异步IO接口，性能远超epoll
// 核心概念：
//   - sq_ring (Submission Queue): 用户提交IO请求的队列
//   - cq_ring (Completion Queue): 内核完成IO后回传结果的队列
//   - 零拷贝、零系统调用：批量提交，批量完成

#include <iostream>
#include <liburing.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

void test_basic_io_uring() {
    std::cout << "=== 测试1: io_uring 基础使用 ===" << std::endl;
    
    // 1. 初始化io_uring
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);  // 队列深度32
    if (ret < 0) {
        std::cerr << "io_uring初始化失败: " << strerror(-ret) << std::endl;
        return;
    }
    std::cout << "✓ io_uring初始化成功" << std::endl;
    
    // 2. 打开文件
    int fd = open("/tmp/test_io_uring.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "文件打开失败" << std::endl;
        io_uring_queue_exit(&ring);
        return;
    }
    
    // 3. 准备写入数据
    const char* msg = "Hello from io_uring!\n";
    struct iovec iov = {
        .iov_base = (void*)msg,
        .iov_len = strlen(msg)
    };
    
    // 4. 获取SQE (Submission Queue Entry)
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "SQE队列已满" << std::endl;
        close(fd);
        io_uring_queue_exit(&ring);
        return;
    }
    
    // 5. 准备writev请求
    io_uring_prep_writev(sqe, fd, &iov, 1, 0);
    
    // 6. 提交请求（真正执行IO）
    io_uring_submit(&ring);
    std::cout << "✓ IO请求已提交" << std::endl;
    
    // 7. 等待完成
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        std::cerr << "等待CQE失败" << std::endl;
    } else {
        std::cout << "✓ IO完成，写入字节数: " << cqe->res << std::endl;
        io_uring_cqe_seen(&ring, cqe);  // 标记CQE已处理
    }
    
    // 8. 清理
    close(fd);
    io_uring_queue_exit(&ring);
    std::cout << "✓ io_uring已销毁" << std::endl;
}

// 对比：传统write() vs io_uring
void compare_sync_vs_async() {
    std::cout << "\n=== 对比：同步IO vs 异步IO ===" << std::endl;
    std::cout << "同步IO (write):" << std::endl;
    std::cout << "  1. 调用write() → 系统调用 → 阻塞等待 → 返回" << std::endl;
    std::cout << "  2. 每次IO都需要系统调用（上下文切换）" << std::endl;
    std::cout << "\n异步IO (io_uring):" << std::endl;
    std::cout << "  1. 用户态准备SQE → io_uring_submit()批量提交" << std::endl;
    std::cout << "  2. 内核异步处理 → 完成后写入CQ → 用户态读取结果" << std::endl;
    std::cout << "  3. 批量操作，极少系统调用" << std::endl;
}

int main() {
    test_basic_io_uring();
    compare_sync_vs_async();
    return 0;
}
