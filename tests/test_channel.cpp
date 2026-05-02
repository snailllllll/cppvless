// 测试4: 自实现Channel（类似Go的channel）
// 编译: g++ -std=c++20 -fcoroutines -o test_channel test_channel.cpp -lpthread
//
// 讲解：
// Go的channel是Go并发模型的核心，C++没有标准实现
// 我们需要自己实现一个，用于：
//   - 协程之间传递数据
//   - 模拟Go的CSP（Communicating Sequential Processes）模型
//   - 在VMess项目中，用于主线程与worker线程通信
//
// 实现思路：
//   - 使用队列存储数据
//   - 使用mutex + condition_variable实现同步
//   - 协程版本：使用coroutine_handle实现无阻塞等待

#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <coroutine>
#include <thread>

// ========== 版本1：线程安全的阻塞Channel ==========
template<typename T>
class BlockingChannel {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
    
public:
    // 发送数据（阻塞直到有空间或channel关闭）
    void send(const T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) throw std::runtime_error("Channel已关闭");
        queue_.push(value);
        cv_.notify_one();  // 通知接收者
    }
    
    // 接收数据（阻塞直到有数据或channel关闭）
    bool recv(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return !queue_.empty() || closed_; });
        
        if (queue_.empty()) return false;  // channel已关闭且无数据
        
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();  // 通知所有等待的线程
    }
};

// ========== 版本2：协程友好的Channel ==========
template<typename T>
class CoroChannel {
private:
    struct Waiter {
        std::coroutine_handle<> h;
        T* value;
        Waiter* next;
    };
    
    std::queue<T> buffer_;
    Waiter* send_waiters_ = nullptr;
    Waiter* recv_waiters_ = nullptr;
    bool closed_ = false;
    
public:
    // 协程版本：尝试发送（如果无接收者则暂停）
    auto async_send(T value) {
        struct Awaitable {
            CoroChannel& ch;
            T value;
            
            bool await_ready() { return false; }
            
            void await_suspend(std::coroutine_handle<> h) {
                // 简化：直接加入缓冲区
                ch.buffer_.push(std::move(value));
                h.resume();  // 立即恢复
            }
            
            void await_resume() {}
        };
        return Awaitable{*this, std::move(value)};
    }
    
    // 协程版本：尝试接收
    auto async_recv(T& out) {
        struct Awaitable {
            CoroChannel& ch;
            T& out;
            
            bool await_ready() { return !ch.buffer_.empty(); }
            
            void await_suspend(std::coroutine_handle<> h) {
                if (!ch.buffer_.empty()) {
                    out = std::move(ch.buffer_.front());
                    ch.buffer_.pop();
                    h.resume();
                }
            }
            
            void await_resume() {}
        };
        return Awaitable{*this, out};
    }
};

// ========== 使用示例 ==========
void test_blocking_channel() {
    std::cout << "=== 测试4.1: 阻塞Channel ===" << std::endl;
    
    BlockingChannel<int> ch;
    
    // 生产者协程
    std::thread producer([&ch]() {
        for (int i = 1; i <= 3; i++) {
            std::cout << "  [Producer] 发送: " << i << std::endl;
            ch.send(i);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ch.close();
    });
    
    // 消费者协程
    std::thread consumer([&ch]() {
        int value;
        while (ch.recv(value)) {
            std::cout << "  [Consumer] 接收: " << value << std::endl;
        }
        std::cout << "  [Consumer] Channel已关闭" << std::endl;
    });
    
    producer.join();
    consumer.join();
}

// ========== 讲解 ==========
void explain_channel() {
    std::cout << "\n=== Channel 设计要点 ===" << std::endl;
    std::cout << "1. 为什么需要Channel？" << std::endl;
    std::cout << "   - Go: '不要通过共享内存来通信，而要通过通信来共享内存'" << std::endl;
    std::cout << "   - 避免锁竞争，提高并发性能" << std::endl;
    
    std::cout << "\n2. 阻塞vs非阻塞" << std::endl;
    std::cout << "   - 阻塞：操作简单，但可能卡住线程" << std::endl;
    std::cout << "   - 非阻塞：立即返回，需要重试逻辑" << std::endl;
    std::cout << "   - 协程版本：暂停当前协程，不阻塞线程！" << std::endl;
    
    std::cout << "\n3. 在VMess项目中的应用" << std::endl;
    std::cout << "   - 主线程 → worker线程：新连接通知" << std::endl;
    std::cout << "   - worker线程 → 主线程：连接关闭通知" << std::endl;
    std::cout << "   - 协程之间：IO完成通知" << std::endl;
}

int main() {
    test_blocking_channel();
    explain_channel();
    return 0;
}
