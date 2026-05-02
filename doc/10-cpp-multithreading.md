# C++ 多线程编程详解

## 1. 多线程基础概念

### 1.1 为什么需要多线程？

**单线程的局限：**
```cpp
// 单线程服务器：处理一个连接时，其他连接必须等待
void SingleThreadServer() {
    while (true) {
        int client_fd = accept(server_fd, ...);  // 阻塞等待连接
        HandleClient(client_fd);  // 处理完这个客户端才能处理下一个
    }
}
```

**多线程的优势：**
- 提高 CPU 利用率（IO 等待时其他线程可运行）
- 提高程序响应速度
- 充分利用多核 CPU

---

## 2. std::thread 基本使用

### 2.1 创建线程

```cpp
#include <iostream>
#include <thread>

// 线程函数
void ThreadFunction(int n) {
    std::cout << "Thread executing, n = " << n << std::endl;
}

int main() {
    // 创建线程
    std::thread t(ThreadFunction, 42);
    
    // 等待线程完成
    t.join();
    
    std::cout << "Main thread exiting" << std::endl;
    return 0;
}
```

### 2.2 使用 Lambda 创建线程

```cpp
#include <iostream>
#include <thread>

int main() {
    int data = 100;
    
    // 使用 lambda 作为线程函数
    std::thread t([data]() mutable {
        for (int i = 0; i < 5; i++) {
            data++;
            std::cout << "Thread: data = " << data << std::endl;
        }
    });
    
    t.join();
    return 0;
}
```

---

## 3. join() vs detach()

### 3.1 join() - 等待线程完成

**作用：** 阻塞当前线程，直到目标线程执行完成。

```cpp
#include <iostream>
#include <thread>
#include <chrono>

void WorkerThread() {
    std::cout << "Worker started" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "Worker finished" << std::endl;
}

int main() {
    std::thread t(WorkerThread);
    
    std::cout << "Main: waiting for worker..." << std::endl;
    t.join();  // 阻塞，直到 worker 完成
    
    std::cout << "Main: worker done, exiting" << std::endl;
    return 0;
}
```

**输出顺序：**
```
Main: waiting for worker...
Worker started
(等待2秒)
Worker finished
Main: worker done, exiting
```

### 3.2 detach() - 分离线程

**作用：** 将线程与 `std::thread` 对象分离，线程在后台独立运行，资源自动回收。

```cpp
#include <iostream>
#include <thread>
#include <chrono>

void DetachedThread() {
    for (int i = 0; i < 5; i++) {
        std::cout << "Detached thread: " << i << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "Detached thread finished" << std::endl;
}

int main() {
    std::thread t(DetachedThread);
    
    t.detach();  // 分离线程，t 不再关联实际线程
    
    // 主线程继续执行
    std::cout << "Main: thread detached, continuing..." << std::endl;
    
    // 主线程等待一段时间，让分离线程有机会执行
    std::this_thread::sleep_for(std::chrono::seconds(6));
    
    std::cout << "Main: exiting" << std::endl;
    return 0;
}
```

**输出（可能）：**
```
Main: thread detached, continuing...
Detached thread: 0
Detached thread: 1
Detached thread: 2
Detached thread: 3
Detached thread: 4
Detached thread finished
Main: exiting
```

### 3.3 关键区别对比

| 特性 | join() | detach() |
|------|--------|----------|
| **线程生命周期管理** | 主线程等待子线程完成 | 子线程独立运行 |
| **资源回收** | 显式等待后回收 | 线程结束时自动回收 |
| **std::thread 对象** | 仍可访问（已 join） | 不再关联任何线程 |
| **使用场景** | 需要等待结果 | 后台任务，无需等待 |
| **安全性** | 更安全，可预测 | 需注意悬空引用 |

### 3.4 必须选择 join 或 detach

**规则：** 每个 `std::thread` 对象在销毁前必须调用 `join()` 或 `detach()`，否则程序会终止。

```cpp
#include <thread>

void Func() {}

int main() {
    std::thread t(Func);
    
    // 错误：未调用 join 或 detach
    // return 0;  // 会调用 std::terminate()
    
    t.join();  // 正确：显式等待
    return 0;
}
```

**错误示例输出：**
```
terminate called without an active exception
Aborted (core dumped)
```

---

## 4. 线程标识和数量

### 4.1 获取线程 ID

```cpp
#include <iostream>
#include <thread>

void PrintThreadId() {
    std::thread::id this_id = std::this_thread::get_id();
    std::cout << "Thread ID: " << this_id << std::endl;
}

int main() {
    std::cout << "Main thread ID: " << std::this_thread::get_id() << std::endl;
    
    std::thread t(PrintThreadId);
    t.join();
    
    return 0;
}
```

### 4.2 设置线程数量

```cpp
#include <iostream>
#include <thread>

int main() {
    // 获取硬件支持的并发线程数
    unsigned int nthreads = std::thread::hardware_concurrency();
    std::cout << "Hardware concurrency: " << nthreads << std::endl;
    
    // 通常设置为 CPU 核心数
    // 对于 IO 密集型任务，可以设置为核心数的 2-4 倍
    
    return 0;
}
```

---

## 5. 线程同步机制

### 5.1 互斥锁 (std::mutex)

**问题：** 多个线程同时访问共享数据会导致数据竞争。

```cpp
#include <iostream>
#include <thread>
#include <vector>

int counter = 0;  // 共享数据

void UnsafeIncrement() {
    for (int i = 0; i < 1000; i++) {
        counter++;  // 数据竞争！
    }
}

int main() {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(UnsafeIncrement);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "Expected: 10000, Actual: " << counter << std::endl;
    // 输出可能小于 10000！
    
    return 0;
}
```

**解决方案：使用互斥锁**

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>

int counter = 0;
std::mutex mtx;  // 互斥锁

void SafeIncrement() {
    for (int i = 0; i < 1000; i++) {
        mtx.lock();    // 加锁
        counter++;      // 临界区
        mtx.unlock();  // 解锁
    }
}

// 更安全的写法：使用 std::lock_guard
void SafeIncrement2() {
    for (int i = 0; i < 1000; i++) {
        std::lock_guard<std::mutex> lock(mtx);  // 构造时加锁，析构时解锁
        counter++;
    }  // 自动解锁
}

int main() {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(SafeIncrement2);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "Expected: 10000, Actual: " << counter << std::endl;
    // 输出一定是 10000
    
    return 0;
}
```

### 5.2 条件变量 (std::condition_variable)

**场景：** 一个线程等待某个条件满足，另一个线程通知它。

```cpp
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

std::queue<int> tasks;
std::mutex mtx;
std::condition_variable cv;
bool done = false;

// 生产者线程
void Producer() {
    for (int i = 0; i < 10; i++) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push(i);
            std::cout << "Produced: " << i << std::endl;
        }
        cv.notify_one();  // 通知消费者
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    {
        std::lock_guard<std::mutex> lock(mtx);
        done = true;
    }
    cv.notify_all();  // 通知所有消费者退出
}

// 消费者线程
void Consumer() {
    while (true) {
        std::unique_lock<std::mutex> lock(mtx);
        
        // 等待条件满足（避免虚假唤醒）
        cv.wait(lock, []() { return !tasks.empty() || done; });
        
        if (done && tasks.empty()) {
            break;  // 退出条件
        }
        
        if (!tasks.empty()) {
            int task = tasks.front();
            tasks.pop();
            lock.unlock();  // 提前解锁
            
            std::cout << "Consumed: " << task << std::endl;
        }
    }
}

int main() {
    std::thread producer(Producer);
    std::thread consumer(Consumer);
    
    producer.join();
    consumer.join();
    
    return 0;
}
```

---

## 6. 原子操作 (std::atomic)

**对于简单数据类型，原子操作比互斥锁更高效。**

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>

std::atomic<int> counter(0);  // 原子变量

void AtomicIncrement() {
    for (int i = 0; i < 1000; i++) {
        counter++;  // 原子操作，无需加锁
    }
}

int main() {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(AtomicIncrement);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "Expected: 10000, Actual: " << counter << std::endl;
    // 输出一定是 10000
    
    return 0;
}
```

---

## 7. VMess 项目中的多线程应用

### 7.1 工作线程模型

**设计目标：** 主线程负责接受连接，工作线程负责处理连接。

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>

class EventLoop;  // 事件循环类

class ThreadPool {
public:
    explicit ThreadPool(int num_threads) : stop_(false) {
        for (int i = 0; i < num_threads; i++) {
            workers_.emplace_back([this, i]() {
                WorkerThread(i);
            });
        }
    }
    
    ~ThreadPool() {
        stop_.store(true);
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
private:
    void WorkerThread(int id) {
        std::cout << "Worker thread " << id << " started" << std::endl;
        
        while (!stop_.load()) {
            // 每个工作线程运行自己的事件循环
            // event_loop_->Run();
            
            // 简化示例：模拟工作
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "Worker thread " << id << " stopped" << std::endl;
    }
    
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_;
};

int main() {
    // 创建线程池（工作线程数 = CPU 核心数）
    int num_threads = std::thread::hardware_concurrency();
    ThreadPool pool(num_threads);
    
    std::cout << "Server started with " << num_threads << " worker threads" << std::endl;
    
    // 主线程继续接受连接
    // AcceptConnections();
    
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    return 0;
}
```

### 7.2 One Loop Per Thread 模型

**这是高性能网络服务器的经典模型（如 Muduo、Netty）。**

```
┌─────────────────────────────────────────┐
│            Main Thread                  │
│    (Accept new connections)             │
└──────────────┬──────────────────────────┘
               │ 分发连接
               ↓
┌─────────────────────────────────────────┐
│       Worker Thread 1 (EventLoop 1)     │  ← 处理连接 1, 3, 5
│       Worker Thread 2 (EventLoop 2)     │  ← 处理连接 2, 4, 6
│       Worker Thread 3 (EventLoop 3)     │  ← 处理连接 7, 8, 9
└─────────────────────────────────────────┘
```

**实现框架：**

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>

// 事件循环类（简化版）
class EventLoop {
public:
    void Run() {
        std::cout << "EventLoop running in thread " 
                  << std::this_thread::get_id() << std::endl;
        
        // 事件循环逻辑
        while (!quit_) {
            // epoll_wait() 等待事件
            // 处理就绪事件
        }
    }
    
    void Quit() { quit_ = true; }
    
private:
    std::atomic<bool> quit_{false};
};

// 线程类（每个线程运行一个 EventLoop）
class EventLoopThread {
public:
    EventLoopThread() : loop_(nullptr) {
        thread_ = std::thread([this]() {
            EventLoop loop;
            loop_ = &loop;
            loop.Run();
        });
    }
    
    ~EventLoopThread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
    EventLoop* GetLoop() const { return loop_; }
    
private:
    std::thread thread_;
    EventLoop* loop_;  // 在线程内创建
};

int main() {
    // 创建多个事件循环线程
    std::vector<std::unique_ptr<EventLoopThread>> threads;
    
    int num_threads = std::thread::hardware_concurrency();
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(std::make_unique<EventLoopThread>());
    }
    
    std::cout << "Server started with " << num_threads << " event loops" << std::endl;
    
    // 主线程继续接受连接并分发到各个 EventLoop
    
    return 0;
}
```

### 7.3 协程 + 多线程

**结合 C++20 协程和多线程：**

```cpp
#include <iostream>
#include <thread>
#include <vector>
#include <coroutine>

// 异步任务
struct Task {
    struct promise_type {
        Task get_return_object() { return Task{this}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
    
    std::coroutine_handle<promise_type> handle;
};

// 在工作线程中执行协程
Task ProcessConnection(int conn_id) {
    std::cout << "Processing connection " << conn_id 
              << " in thread " << std::this_thread::get_id() << std::endl;
    
    // 模拟异步 IO
    co_await std::suspend_always{};
    
    std::cout << "Connection " << conn_id << " processed" << std::endl;
}

int main() {
    // 创建线程池执行协程
    std::vector<std::thread> workers;
    
    for (int i = 0; i < 4; i++) {
        workers.emplace_back([i]() {
            ProcessConnection(i);
        });
    }
    
    for (auto& t : workers) {
        t.join();
    }
    
    return 0;
}
```

---

## 8. detach() 在协程代码中的含义

### 8.1 回顾你的协程代码

```cpp
std::thread([handle]() mutable {
    handle();  // 恢复协程执行
}).detach();
```

**这里 detach() 的作用：**

1. **创建新线程** 来执行协程的恢复操作
2. **分离线程**：主线程不需要等待这个协程线程完成
3. **后台运行**：协程在后台独立执行，直到完成或挂起

**为什么可以 detach？**
- 协程有自己的生命周期管理（通过 `coroutine_handle`）
- 线程函数执行完毕后，资源自动回收
- 不需要主线程等待结果

### 8.2 更安全的做法

```cpp
// 使用 joinable() 检查
std::thread t([handle]() mutable {
    handle();
});

if (t.joinable()) {
    t.detach();  // 或者 t.join()
}
```

---

## 9. 最佳实践和注意事项

### 9.1 避免的问题

**1. 数据竞争**
```cpp
// 错误：多个线程同时写
int shared_data = 0;
std::thread t1([&]() { shared_data++; });
std::thread t2([&]() { shared_data++; });

// 正确：使用互斥锁或原子变量
std::mutex mtx;
std::thread t1([&]() { 
    std::lock_guard<std::mutex> lock(mtx);
    shared_data++; 
});
```

**2. 死锁**
```cpp
// 错误：多个锁的获取顺序不一致
std::mutex mtx1, mtx2;

void Thread1() {
    std::lock_guard<std::mutex> l1(mtx1);
    std::lock_guard<std::mutex> l2(mtx2);  // 可能死锁
}

void Thread2() {
    std::lock_guard<std::mutex> l2(mtx2);
    std::lock_guard<std::mutex> l1(mtx1);  // 可能死锁
}

// 正确：使用 std::lock 同时锁多个互斥量
void SafeLock() {
    std::unique_lock<std::mutex> l1(mtx1, std::defer_lock);
    std::unique_lock<std::mutex> l2(mtx2, std::defer_lock);
    std::lock(l1, l2);  // 避免死锁
}
```

**3. 悬空引用**
```cpp
// 错误：线程访问已销毁的局部变量
std::thread CreateThread() {
    int local = 42;
    return std::thread([&local]() {
        std::cout << local << std::endl;  // 危险！
    });
}

// 正确：值捕获
std::thread CreateThread() {
    int local = 42;
    return std::thread([local]() {  // 拷贝 local
        std::cout << local << std::endl;
    });
}
```

### 9.2 线程数建议

```cpp
// CPU 密集型：线程数 = CPU 核心数
int cpu_bound_threads = std::thread::hardware_concurrency();

// IO 密集型：线程数可以是核心数的 2-4 倍
int io_bound_threads = std::thread::hardware_concurrency() * 2;
```

---

## 10. 总结

### 10.1 关键概念

| 概念 | 说明 |
|------|------|
| **std::thread** | C++11 提供的线程类 |
| **join()** | 等待线程完成 |
| **detach()** | 分离线程，后台运行 |
| **std::mutex** | 互斥锁，保护共享数据 |
| **std::condition_variable** | 条件变量，线程间通信 |
| **std::atomic** | 原子操作，无锁编程 |
| **std::lock_guard** | RAII 风格的锁管理 |

### 10.2 VMess 项目多线程方案

**推荐：One Loop Per Thread + 线程池**

```
Main Thread (Accept) → 分发连接
    ↓
Worker Thread 1 (EventLoop) → 处理部分连接
Worker Thread 2 (EventLoop) → 处理部分连接
Worker Thread 3 (EventLoop) → 处理部分连接
```

**优势：**
- 充分利用多核 CPU
- 避免锁竞争（每个线程独立事件循环）
- 支持协程异步编程
- 易于扩展和调试

### 10.3 detach() 使用场景

✅ **适合 detach 的场景：**
- 后台任务（日志写入、数据统计）
- 协程恢复操作
- 无需等待结果的任务

❌ **不适合 detach 的场景：**
- 需要获取线程执行结果
- 线程访问主线程资源
- 需要顺序控制的任务
