# pants

参考 [pans](https://gitee.com/huajianke2026/pans) 制作的现代 C++ 网络服务器（in progress）。

## 特性

- 多 Reactor + 多线程 + 有栈协程（fiber）
- 基于 C++23 modules，仅支持 Linux
- CMake + Ninja 构建（GCC 15 / Clang 23）

## 模块

| 模块 | 说明 |
|---|---|
| fiber | 有栈协程，支持专用栈与共享栈（dedicated / eager / lazy 三种策略） |
| scheduler | 多线程调度器，root / idle / callback fiber 使用专用栈 |
| timer | 基于时间轮的定时器 |
| iomanager | epoll 事件循环与 fd 事件管理 |
| fd_manager | fd 封装 |
| hook | 同步 IO 的协程化 hook |
| config | 配置管理 |
| mutex | 协程安全的互斥量 |
| log | 异步日志 |
| utility | 时间、文本、文件系统、线程等通用工具 |

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

## 测试

```bash
ctest --test-dir build
```

## 状态

网络层（socket / listener / connection）尚未开始，其余模块可用。

## License

[MIT](LICENSE)
