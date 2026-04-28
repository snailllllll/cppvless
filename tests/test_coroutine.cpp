// 测试C++20协程支持
#include <iostream>
#include <coroutine>

// 简单的Task类型，用于测试协程语法
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

// 协程函数
Task hello_coroutine() {
    std::cout << "  [协程] 开始执行" << std::endl;
    co_return;
    std::cout << "  [协程] 这行不会执行" << std::endl;
}

int main() {
    std::cout << "=== C++20协程测试 ===" << std::endl;
    
    std::cout << "[主函数] 调用协程前" << std::endl;
    hello_coroutine();
    std::cout << "[主函数] 调用协程后" << std::endl;
    
    std::cout << "\n✅ C++20协程语法支持正常！" << std::endl;
    return 0;
}
