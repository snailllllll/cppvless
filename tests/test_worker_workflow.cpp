// 工作线程工作流程示例
// 演示：主线程生产连接 → 工作线程消费 → io_uring批量注册 → 协程处理（IO挂起/恢复）
//
// 编译: g++ -std=c++20 -fcoroutines -o test_worker_workflow test_worker_workflow.cpp -luring -lpthread
//
// 流程说明：
// 1. 主线程accept连接，放入SPSC无锁队列
// 2. 工作线程从队列取出连接，注册io_uring读取事件
// 3. io_uring等待完成事件
// 4. 对每个有数据的连接，启动协程处理
// 5. 协程流程：处理阶段1 → IO转发远端 → IO读取返回 → 处理阶段2 → 发送回客户端
// 6. IO操作挂起协程，工作线程继续调度其他协程

#include <iostream>
#include <coroutine>
#include <thread>
#include <vector>
#include <memory>
#include <unistd.h>
#include <liburing.h>

// ========= 1. SPSC无锁队列（单生产者单消费者）==========

template<typename T>
class SPSCQueue {
private:
    struct Node {
        T data;
        Node* next;
        Node(T&& val) : data(std::move(val)), next(nullptr) {}
    };
    
    std::atomic<Node*> head_;  // 消费者读取端
    std::atomic<Node*> tail_;  // 生产者写入端
    Node dummy_;
    
public:
    SPSCQueue() : dummy_(T()), head_(&dummy_), tail_(&dummy_) {}
    
    ~SPSCQueue() {
        T temp;
        while (pop(temp)) {}
        delete head_.load();
    }
    
    // 生产者调用：入队（无锁）
    void push(T&& value) {
        Node* node = new Node(std::move(value));
        
        // 获取当前tail，将它的next指向新节点
        Node* old_tail = tail_.load(std::memory_order_relaxed);
        old_tail->next = node;
        
        // 移动tail指针
        tail_.store(node, std::memory_order_release);
    }
    
    // 消费者调用：出队（无锁）
    bool pop(T& result) {
        Node* head = head_.load(std::memory_order_relaxed);
        Node* next = head->next;
        
        if (next == nullptr) {
            return false;  // 队列空
        }
        
        // 读取数据
        result = std::move(next->data);
        
        // 移动head指针
        head_.store(next, std::memory_order_release);
        
        // 删除旧节点
        delete head;
        
        return true;
    }
};

// ========= 2. 连接对象 =========

struct Connection {
    int client_fd;   // 客户端fd
    int remote_fd;   // 远端fd（简化：实际应该动态创建）
    
    Connection(int client, int remote) 
        : client_fd(client), remote_fd(remote) {}
};

// ========= 3. 协程返回类型 =========

struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
    
    std::coroutine_handle<promise_type> handle;
    
    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() { if(handle) handle.destroy(); }
};

// ========= 4. 异步IO操作（返回awaitable对象）==========

// 异步读取的Awaitable
struct AsyncReadAwaitable {
    int fd;
    std::vector<uint8_t>& buffer;
    
    AsyncReadAwaitable(int f, std::vector<uint8_t>& buf) : fd(f), buffer(buf) {}
    
    bool await_ready() { return false; }
    
    void await_suspend(std::coroutine_handle<> h) {
        // 简化：立即恢复（实际应将IO请求提交到io_uring）
        std::cout << "    [IO] 提交读取请求 fd=" << fd << std::endl;
        h.resume();
    }
    
    void await_resume() {
        // 模拟读取到数据
        buffer.resize(100);
        std::fill(buffer.begin(), buffer.end(), 'A');
        std::cout << "    [IO] 读取完成 fd=" << fd << " 大小=" << buffer.size() << std::endl;
    }
};

// 异步写入的Awaitable
struct AsyncWriteAwaitable {
    int fd;
    const std::vector<uint8_t>& buffer;
    
    AsyncWriteAwaitable(int f, const std::vector<uint8_t>& buf) : fd(f), buffer(buf) {}
    
    bool await_ready() { return false; }
    
    void await_suspend(std::coroutine_handle<> h) {
        std::cout << "    [IO] 提交写入请求 fd=" << fd << " 大小=" << buffer.size() << std::endl;
        h.resume();
    }
    
    void await_resume() {
        std::cout << "    [IO] 写入完成 fd=" << fd << std::endl;
    }
};

// 异步读取函数
AsyncReadAwaitable async_read(int fd, std::vector<uint8_t>& buffer) {
    return AsyncReadAwaitable(fd, buffer);
}

// 异步写入函数
AsyncWriteAwaitable async_write(int fd, const std::vector<uint8_t>& buffer) {
    return AsyncWriteAwaitable(fd, buffer);
}

// ========= 5. 连接处理协程 =========

Task handle_connection(Connection* conn) {
    std::cout << "  [协程] 开始处理连接 client_fd=" << conn->client_fd << std::endl;
    
    std::vector<uint8_t> buffer;
    
    // 处理阶段1（简化）
    std::cout << "  [协程] 处理阶段1：解析VMess协议..." << std::endl;
    co_await std::suspend_always{};  // 模拟处理
    
    // IO转发到远端
    co_await async_read(conn->client_fd, buffer);
    co_await async_write(conn->remote_fd, buffer);
    
    // IO拿到远端的返回
    std::vector<uint8_t> response;
    co_await async_read(conn->remote_fd, response);
    
    // 处理阶段2（简化）
    std::cout << "  [协程] 处理阶段2：加密响应..." << std::endl;
    co_await std::suspend_always{};  // 模拟处理
    
    // 发送回客户端
    co_await async_write(conn->client_fd, response);
    
    std::cout << "  [协程] 连接处理完成" << std::endl;
    co_return;
}

// ========= 6. 工作线程 =========

class Worker {
private:
    SPSCQueue<std::unique_ptr<Connection>> queue_;
    io_uring ring_;
    std::vector<std::coroutine_handle<>> active_coroutines_;
    
public:
    void run() {
        std::cout << "[Worker] 启动，初始化io_uring..." << std::endl;
        
        // 初始化io_uring
        io_uring_queue_init(32, &ring_, 0);
        
        std::cout << "[Worker] 进入事件循环..." << std::endl;
        
        // 简化：只处理3个模拟连接
        int processed = 0;
        const int max_connections = 3;
        
        while (processed < max_connections) {
            // 1. 从队列取出新连接
            std::unique_ptr<Connection> conn;
            while (queue_.pop(conn)) {
                std::cout << "\n[Worker] 收到新连接 client_fd=" << conn->client_fd << std::endl;
                
                // 注册io_uring读取事件（简化：实际应调用io_uring_prep_read）
                std::cout << "[Worker] 注册io_uring读取事件 fd=" << conn->client_fd << std::endl;
                
                // 启动协程处理
                Task task = handle_connection(conn.get());
                std::cout << "[Worker] 启动协程处理连接" << std::endl;
                
                // 恢复协程（实际应在io_uring完成事件中恢复）
                if (!task.handle.done()) {
                    std::cout << "[Worker] 恢复协程..." << std::endl;
                    task.handle.resume();
                    active_coroutines_.push_back(task.handle);
                }
            }
            
            // 2. 等待io_uring完成事件（简化）
            std::cout << "[Worker] 等待io_uring完成事件..." << std::endl;
            sleep(1);  // 模拟等待
            
            // 3. 恢复已挂起的协程（简化：实际应从CQE获取）
            for (auto it = active_coroutines_.begin(); it != active_coroutines_.end(); ) {
                if (!(*it).done()) {
                    std::cout << "[Worker] 恢复挂起的协程..." << std::endl;
                    (*it).resume();
                }
                
                if ((*it).done()) {
                    std::cout << "[Worker] 协程已完成，清理" << std::endl;
                    (*it).destroy();
                    it = active_coroutines_.erase(it);
                    processed++;
                } else {
                    ++it;
                }
            }
        }
        
        std::cout << "\n[Worker] 所有连接处理完成，退出" << std::endl;
        io_uring_queue_exit(&ring_);
    }
    
    // 主线程调用：添加连接
    void add_connection(std::unique_ptr<Connection> conn) {
        queue_.push(std::move(conn));
        
        // 唤醒工作线程（简化：实际应提交NOP到io_uring）
        std::cout << "[Main] 添加连接到队列 client_fd=" << conn->client_fd << std::endl;
    }
};

// ========= 7. 主函数 =========

int main() {
    std::cout << "=== 测试：工作线程工作流程 ===\n" << std::endl;
    
    Worker worker;
    
    // 启动工作线程
    std::thread worker_thread([&worker]() {
        worker.run();
    });
    
    // 主线程：模拟accept连接并放入队列
    std::cout << "[Main] 模拟接受连接...\n" << std::endl;
    
    sleep(1);  // 等待工作线程启动
    
    // 添加3个模拟连接
    for (int i = 1; i <= 3; i++) {
        auto conn = std::make_unique<Connection>(i, i + 100);
        worker.add_connection(std::move(conn));
        sleep(1);
    }
    
    // 等待工作线程完成
    worker_thread.join();
    
    std::cout << "\n=== 测试完成 ===" << std::endl;
    return 0;
}
