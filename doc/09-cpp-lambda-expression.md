# C++ Lambda 表达式详解

## 1. 基本概念

Lambda 表达式是 C++11 引入的匿名函数特性，允许在需要函数的地方内联定义函数。

### 1.1 基本语法

```cpp
[capture](parameters) -> return_type { function_body }
```

**完整示例：**
```cpp
#include <iostream>
#include <vector>
#include <algorithm>

int main() {
    std::vector<int> nums = {1, 2, 3, 4, 5};
    
    // 基本 lambda：打印每个元素
    std::for_each(nums.begin(), nums.end(), [](int x) {
        std::cout << x << " ";
    });
    // 输出：1 2 3 4 5
    
    return 0;
}
```

---

## 2. 捕获子句 (Capture Clause)

捕获子句 `[capture]` 决定 lambda 如何访问外部变量。

### 2.1 值捕获 (by value)

```cpp
int a = 10, b = 20;

// 值捕获：拷贝外部变量
auto lambda1 = [a, b]() {
    std::cout << a << " " << b << std::endl;
    // a 和 b 是拷贝，外部修改不影响这里
};

a = 100;  // 修改外部变量
lambda1(); // 输出：10 20（不受外部修改影响）
```

### 2.2 引用捕获 (by reference)

```cpp
int a = 10, b = 20;

// 引用捕获：直接引用外部变量
auto lambda2 = [&a, &b]() {
    std::cout << a << " " << b << std::endl;
};

a = 100;  // 修改外部变量
lambda2(); // 输出：100 20（受外部修改影响）
```

### 2.3 隐式捕获

```cpp
int x = 10, y = 20, z = 30;

// [=]：所有变量按值捕获
auto lambda3 = [=]() {
    std::cout << x << " " << y << " " << z << std::endl;
};

// [&]：所有变量按引用捕获
auto lambda4 = [&]() {
    x = 100;  // 修改外部变量
    std::cout << x << std::endl;
};

// [=, &x]：混合捕获（y,z 按值，x 按引用）
auto lambda5 = [=, &x]() {
    x = 200;  // 可以修改 x
    // y = 200;  // 错误：y 是值捕获，默认不可修改
};
```

---

## 3. mutable 关键字详解

### 3.1 问题背景

**默认情况下，值捕获的变量在 lambda 内部是 const 的。**

```cpp
int count = 0;

auto lambda = [count]() {
    count++;  // 编译错误！count 是 const 拷贝
    std::cout << count << std::endl;
};
```

### 3.2 mutable 的作用

**`mutable` 允许修改值捕获的变量（修改的是拷贝，不影响原始变量）。**

```cpp
#include <iostream>

int main() {
    int count = 0;
    
    // 使用 mutable：允许修改值捕获的拷贝
    auto lambda = [count]() mutable {
        count++;  // 正确：修改的是 count 的拷贝
        std::cout << "Inside lambda: " << count << std::endl;
    };
    
    lambda();      // 输出：Inside lambda: 1
    lambda();      // 输出：Inside lambda: 2
    
    std::cout << "Outside: " << count << std::endl;
    // 输出：Outside: 0（原始变量未被修改）
    
    return 0;
}
```

### 3.3 mutable 的本质

**`mutable` 实际上是让 lambda 的 `operator()` 变成非 const 成员函数。**

```cpp
// 编译器会将 lambda 转换为类似这样的函数对象：
class Lambda {
private:
    int count;  // 值捕获的变量变成成员变量
    
public:
    Lambda(int c) : count(c) {}
    
    // 没有 mutable：operator() 是 const
    // void operator()() const { count++; }  // 错误
    
    // 有 mutable：operator() 是非 const
    void operator()() { count++; }  // 正确
};
```

### 3.4 实际应用场景

#### 场景1：状态累积

```cpp
#include <iostream>
#include <vector>
#include <algorithm>

int main() {
    std::vector<int> nums = {1, 2, 3, 4, 5};
    int sum = 0;
    
    // 使用 mutable 累积状态
    std::for_each(nums.begin(), nums.end(), [sum](int x) mutable {
        sum += x;
        std::cout << "Current sum: " << sum << std::endl;
    });
    
    std::cout << "Final sum: " << sum << std::endl;
    // 注意：这里的 sum 是拷贝，外部 sum 仍然是 0
    
    return 0;
}
```

#### 场景2：协程中的状态管理（回到你的代码）

```cpp
// 你之前看到的协程代码：
std::thread([handle]() mutable {
    handle();  // 需要修改 handle 状态（调用协程）
}).detach();

// 为什么需要 mutable？
// 因为 handle 是值捕获，调用 handle() 会修改协程状态
// 如果没有 mutable，operator() 是 const，无法调用非 const 的 handle()
```

---

## 4. 参数列表和返回类型

### 4.1 参数列表

```cpp
// 带参数的 lambda
auto add = [](int a, int b) -> int {
    return a + b;
};

std::cout << add(10, 20) << std::endl;  // 输出：30
```

### 4.2 返回类型推导

```cpp
// C++14 开始支持返回类型自动推导
auto lambda1 = [](int x) {
    return x * 2;  // 自动推导返回类型为 int
};

auto lambda2 = [](int x) -> double {
    return x * 1.5;  // 显式指定返回类型为 double
};
```

---

## 5. 泛型 Lambda (C++14)

```cpp
// 参数类型自动推导
auto generic = [](auto x, auto y) {
    return x + y;
};

std::cout << generic(10, 20) << std::endl;     // int
std::cout << generic(1.5, 2.3) << std::endl;   // double
std::cout << generic(std::string("Hello"), std::string(" World")) << std::endl;  // string
```

---

## 6. 项目中的应用示例

### 6.1 事件回调

```cpp
// VMess 项目中的 Channel 回调
class Channel {
private:
    std::function<void()> read_callback_;
    std::function<void()> write_callback_;
    
public:
    void SetReadCallback(std::function<void()> cb) {
        read_callback_ = cb;
    }
    
    void HandleEvent() {
        if (read_callback_) {
            read_callback_();
        }
    }
};

// 使用 lambda 作为回调
Channel channel;
channel.SetReadCallback([this]() {
    this->OnMessage();  // 捕获 this 指针
});
```

### 6.2 多线程中的 Lambda

```cpp
#include <thread>
#include <iostream>

int main() {
    int data = 42;
    
    // lambda 作为线程函数
    std::thread t([data]() mutable {
        for (int i = 0; i < 5; i++) {
            data++;
            std::cout << "Thread: " << data << std::endl;
        }
    });
    
    t.join();
    std::cout << "Main: " << data << std::endl;  // data 仍然是 42
    
    return 0;
}
```

---

## 7. 最佳实践

### 7.1 避免悬空引用

```cpp
// 错误示例：捕获局部变量的引用
auto CreateLambda() {
    int x = 10;
    return [&x]() {  // 危险：x 在函数返回后被销毁
        std::cout << x << std::endl;
    };
}

// 正确做法：使用值捕获
auto CreateLambda() {
    int x = 10;
    return [x]() {  // 安全：拷贝 x
        std::cout << x << std::endl;
    };
}
```

### 7.2 优先使用 const 引用捕获

```cpp
std::string large_string = "Very large string...";

// 避免拷贝大型对象
auto lambda = [&large_string]() {  // 引用捕获
    std::cout << large_string << std::endl;
};

// 或者 C++14 初始化捕获
auto lambda = [str = large_string]() {  // 移动语义
    std::cout << str << std::endl;
};
```

---

## 8. 总结

| 特性 | 语法 | 说明 |
|------|------|------|
| 值捕获 | `[x]` | 拷贝变量，mutable 下可修改拷贝 |
| 引用捕获 | `[&x]` | 引用变量，修改影响外部 |
| 隐式值捕获 | `[=]` | 所有变量按值捕获 |
| 隐式引用捕获 | `[&]` | 所有变量按引用捕获 |
| **mutable** | `[x]() mutable {}` | **允许修改值捕获的拷贝** |
| 泛型 lambda | `[](auto x) {}` | C++14 参数类型自动推导 |

**关键点：**
- `mutable` 让值捕获的变量可在 lambda 内部修改（修改的是拷贝）
- 若不 `mutable`，值捕获变量在 lambda 内是 `const`
- 引用捕获不受 `mutable` 影响（本来就可以修改）
- 协程场景中常用 `mutable` 来调用 `coroutine_handle::resume()`
