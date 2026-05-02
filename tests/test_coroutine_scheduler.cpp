// 测试3: 协程调度器（简化版）
// 编译: g++ -std=c++20 -fcoroutines -o test_coroutine_scheduler test_coroutine_scheduler.cpp -lpthread
//
// 讲解：
// 协程调度器是异步编程的核心，负责：
//   1. 管理所有协程的生命周期
//   2. 决定哪个协程下一个运行
//   3. 处理协程的暂停、恢复、销毁
//
// 在VMess项目中的角色：
//   - 每个worker线程有一个调度器
//   - 调度器从io_uring获取完成的IO事件
//   - 恢复等待这些IO事件的协程
//
// 本示例实现一个简化的调度器，帮助你理解原理

#include <iostream>
#include <coroutine>
#include <queue>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

// ========== 1. Task类型（可等待的协程返回类型）==========
struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }  // 创建后暂停
        std::suspend_always final_suspend() noexcept { return {}; }  // 结束前暂停
        void return_void() {}
        void unhandled_exception() {}
    };
    
    std::coroutine_handle<promise_type> handle;  // 协程句柄
    
    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() { if(handle) handle.destroy(); }
    
    // 等待这个协程完成
    void wait() {
        if (handle && !handle.done()) {
            handle.resume();
        }
    }
};

// ========== 2. 简化的协程调度器 ==========
class CoroutineScheduler {
private:
    std::queue<std::coroutine_handle<>> ready_queue;  // 就绪队列
    std::mutex queue_mutex;
    
public:
    // 将协程加入就绪队列
    void schedule(std::coroutine_handle<> h) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        ready_queue.push(h);
        std::cout << "  [Scheduler] 协程已加入就绪队列" << std::endl;
    }
    
    // 运行所有就绪的协程
    void run() {
        std::cout << "  [Scheduler] 开始调度..." << std::endl;
        
        while (!ready_queue.empty()) {
            std::coroutine_handle<> h;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                h = ready_queue.front();
                ready_queue.pop();
            }
            
            if (!h.done()) {
                std::cout << "  [Scheduler] 恢复协程..." << std::endl;
                h.resume();  // 恢复协程执行
                
                // 如果协程暂停了（不是done），重新加入队列
                if (!h.done()) {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    ready_queue.push(h);
                }
            }
        }
        
        std::cout << "  [Scheduler] 所有协程已完成" << std::endl;
    }
};

// ========== 3. 示例使用 ==========
// 模拟异步操作（比如io_uring的IO完成）
Task async_read(int fd) {
    std::cout << "  [async_read] 开始读取文件" << std::endl;
    co_await std::suspend_always{};  // 模拟IO等待
    std::cout << "  [async_read] IO完成，恢复协程" << std::endl;
    co_return;
}

Task async_write(int fd) {
    std::cout << "  [async_write] 开始写入文件" << std::endl;
    co_await std::suspend_always{};  // 模拟IO等待
    std::cout << "  [async_write] IO完成，恢复协程" << std::endl;
    co_return;
}

// ========== 4. 主函数 ==========
int main() {
    std::cout << "=== 测试3: 协程调度器 ===" << std::endl;
    
    CoroutineScheduler scheduler;
    
    // 创建协程
    std::cout << "\n[Main] 创建协程1 (async_read)" << std::endl;
    Task t1 = async_read(1);
    scheduler.schedule(t1.handle);
    
    std::cout << "\n[Main] 创建协程2 (async_write)" << std::endl;
    Task t2 = async_write(2);
    scheduler.schedule(t2.handle);
    
    // 运行调度器
    std::cout << "\n--- 开始调度 ---" << std::endl;
    scheduler.run();
    
    // 讲解
    std::cout << "\n=== 协程调度器原理 ===" << std::endl;
    std::cout << "1. 协程创建后暂停 (initial_suspend)" << std::endl;
    std::cout << "2. 调度器将协程加入就绪队列" << std::endl;
    std::cout << "3. 调度器恢复协程执行 (handle.resume())" << std::endl;
    std::cout << "4. 协程遇到co_await暂停，让出CPU" << std::endl;
    std::cout << "5. 调度器继续调度其他就绪协程" << std::endl;
    std::cout << "6. 所有协程完成后，调度器退出" << std::endl;
    
    return 0;
}
