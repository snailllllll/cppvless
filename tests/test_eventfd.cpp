// 测试2: eventfd 线程间通信
// 编译: g++ -std=c++20 -o test_eventfd test_eventfd.cpp -lpthread
//
// 讲解：
// eventfd 是Linux 2.6.22+的线程/进程间通信机制
// 优势（相比pipe）：
//   1. 只用一个文件描述符（pipe需要2个）
//   2. 内核级支持，无数据拷贝
//   3. 与epoll/io_uring完美集成
//   4. 8字节整数传递，语义清晰
//
// 在VMess项目中的用途：
//   - 主线程通知工作线程有新连接
//   - 工作线程通知主线程需要关闭连接
//   - 协程调度器唤醒等待的协程

#include <iostream>
#include <thread>
#include <unistd.h>
#include <sys/eventfd.h>
#include <poll.h>

// 示例1：基础使用
void test_basic_eventfd() {
    std::cout << "=== 测试2.1: eventfd 基础 ===" << std::endl;
    
    // 创建eventfd
    // EFD_NONBLOCK: 非阻塞
    // EFD_CLOEXEC: exec时关闭
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        std::cerr << "eventfd创建失败" << std::endl;
        return;
    }
    std::cout << "✓ eventfd创建成功, fd=" << efd << std::endl;
    
    // 写入数据（通知其他线程）
    uint64_t value = 1;  // 必须8字节整数
    write(efd, &value, sizeof(value));
    std::cout << "✓ 写入通知: " << value << std::endl;
    
    // 读取数据（消费通知）
    uint64_t result;
    read(efd, &result, sizeof(result));
    std::cout << "✓ 读取通知: " << result << std::endl;
    
    close(efd);
}

// 示例2：线程间通信
void test_thread_communication() {
    std::cout << "\n=== 测试2.2: eventfd 线程通信 ===" << std::endl;
    
    int efd = eventfd(0, EFD_NONBLOCK);
    
    std::thread worker([efd]() {
        std::cout << "  [Worker] 等待主线程通知..." << std::endl;
        
        uint64_t value;
        read(efd, &value, sizeof(value));
        
        std::cout << "  [Worker] 收到通知: " << value 
                  << ", 开始处理..." << std::endl;
    });
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "[Main] 通知工作线程" << std::endl;
    
    uint64_t notify = 42;
    write(efd, &notify, sizeof(notify));
    
    worker.join();
    close(efd);
}

// 示例3：与poll/epoll集成
void test_with_poll() {
    std::cout << "\n=== 测试2.3: eventfd + poll ===" << std::endl;
    
    int efd = eventfd(0, EFD_NONBLOCK);
    
    std::thread notifier([efd]() {
        sleep(2);
        uint64_t value = 100;
        write(efd, &value, sizeof(value));
        std::cout << "  [Notifier] 已发送通知" << std::endl;
    });
    
    std::cout << "等待eventfd可读..." << std::endl;
    
    struct pollfd pfd = {efd, POLLIN, 0};
    int ret = poll(&pfd, 1, 3000);  // 等待3秒
    
    if (ret > 0 && (pfd.revents & POLLIN)) {
        uint64_t value;
        read(efd, &value, sizeof(value));
        std::cout << "✓ 收到poll通知: " << value << std::endl;
    }
    
    notifier.join();
    close(efd);
}

// 讲解：为什么eventfd比pipe好？
void explain_why_eventfd() {
    std::cout << "\n=== eventfd vs pipe ===" << std::endl;
    std::cout << "pipe劣势:" << std::endl;
    std::cout << "  1. 需要2个fd (read端 + write端)" << std::endl;
    std::cout << "  2. 需要数据拷贝 (write buffer → 内核 → read buffer)" << std::endl;
    std::cout << "  3. 传递小数据时开销大" << std::endl;
    
    std::cout << "\neventfd优势:" << std::endl;
    std::cout << "  1. 只需1个fd" << std::endl;
    std::cout << "  2. 无数据拷贝，只传递8字节整数" << std::endl;
    std::cout << "  3. 内核直接操作，性能极高" << std::endl;
    std::cout << "  4. 与epoll/io_uring完美集成" << std::endl;
}

int main() {
    test_basic_eventfd();
    test_thread_communication();
    test_with_poll();
    explain_why_eventfd();
    return 0;
}
