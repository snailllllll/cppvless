// VMess代理服务器主程序入口
import std;

int main(int argc, char* argv[]) {
    std::cout << "VMess C++ Proxy Server" << std::endl;
    std::cout << "基于 io_uring + C++20协程" << std::endl;
    std::cout << "构建类型: " << 
        #ifdef NDEBUG
        "Release"
        #else
        "Debug"
        #endif
        << std::endl;
    
    // TODO: 初始化日志系统
    // TODO: 加载配置文件
    // TODO: 启动主事件循环
    
    return 0;
}
