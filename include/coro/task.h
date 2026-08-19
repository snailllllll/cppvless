#ifndef VMESS_CORO_TASK_H
#define VMESS_CORO_TASK_H

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace vmess {
namespace coro {

// 前置声明
template<typename T>
struct Task;

namespace detail {

// Task 的 promise_type
template<typename T>
struct TaskPromise {
    std::optional<T> value;
    std::exception_ptr exception;
    std::coroutine_handle<> continuation;

    Task<T> get_return_object(); 

    std::suspend_always initial_suspend() { return {}; }

    auto final_suspend() noexcept {
        struct FinalAwaitable {
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<TaskPromise<T>> h) noexcept {
                auto& promise = h.promise();
                if (promise.continuation) {
                    promise.continuation.resume();
                }
            }
            void await_resume() noexcept {}
        };
        return FinalAwaitable{};
    }

    void return_value(T v) {
        value = std::move(v);
    }

    void unhandled_exception() {
        exception = std::current_exception();
    }
};

// void 特化
template<>
struct TaskPromise<void> {
    std::exception_ptr exception;
    std::coroutine_handle<> continuation;

    Task<void> get_return_object();

    std::suspend_always initial_suspend() { return {}; }

    auto final_suspend() noexcept {
        struct FinalAwaitable {
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<TaskPromise<void>> h) noexcept {
                auto& promise = h.promise();
                if (promise.continuation) {
                    promise.continuation.resume();
                }
            }
            void await_resume() noexcept {}
        };
        return FinalAwaitable{};
    }

    void return_void() {}

    void unhandled_exception() {
        exception = std::current_exception();
    }
};

} // namespace detail

// Task<T>: 可等待的协程返回类型
template<typename T>
struct Task {
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    handle_type h;

    explicit Task(handle_type handle) : h(handle) {}

    Task() = default;

    ~Task() {
        if (h) {
            h.destroy();
        }
    }

    // 禁用拷贝
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 支持移动
    Task(Task&& other) noexcept : h(other.h) {
        other.h = nullptr;
    }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (h) h.destroy();
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }

    bool done() const {
        return !h || h.done();
    }

    // 作为 awaitable
    bool await_ready() const {
        return done();
    }

    void await_suspend(std::coroutine_handle<> caller) {
        h.promise().continuation = caller;
        h.resume();
    }

    T await_resume() {
        if (h.promise().exception) {
            std::rethrow_exception(h.promise().exception);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*h.promise().value);
        }
    }

    // 获取结果（非协程调用）
    T result() {
        if (!h.done()) {
            h.resume();
        }
        if (h.promise().exception) {
            std::rethrow_exception(h.promise().exception);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*h.promise().value);
        }
    }
};

// promise_type 的 get_return_object 定义（必须在 Task 定义之后）
template<typename T>
Task<T> detail::TaskPromise<T>::get_return_object() {
    return Task<T>{
        std::coroutine_handle<TaskPromise<T>>::from_promise(*this)
    };
}

inline Task<void> detail::TaskPromise<void>::get_return_object() {
    return Task<void>{
        std::coroutine_handle<TaskPromise<void>>::from_promise(*this)
    };
}

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_TASK_H
