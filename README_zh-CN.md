**语言：** [English](README.md) | 简体中文

# shmipc — 共享内存 IPC 库

基于共享内存（`memfd` + `mmap`）的高性能双向 IPC 框架，纯 C 公开 API，内部 C++11 实现，支持 Linux x86_64 与 Android arm64-v8a。

> **一句话介绍：** 基于 futex 通知的零拷贝共享内存管道，提供简洁的 C 语言 API，专为 Linux 和 Android 上的高吞吐、低延迟本地 IPC 设计。

---

## 特性

| 特性 | 说明 |
|------|------|
| **零拷贝接收** | 单 slice 消息直接借用 SHM 指针，`on_data_zc` 无堆拷贝 |
| **零拷贝写入** | `alloc_buf` / `send_buf` 让调用方原地填写 SHM slice，省去内部 `memcpy` |
| **futex 通知** | `FUTEX_WAIT/WAKE` 替代 UDS 数据通知，减少上下文切换 |
| **全双工** | `server_write` / `client_write` 独立 ring buffer，两侧并发无竞争 |
| **崩溃感知** | UDS 连接断开时自动触发 `on_disconnected` 并释放共享内存 |
| **背压控制** | 写入支持阻塞、非阻塞（丢弃）、定时三种模式 |
| **异步 dispatch** | 可选独立 dispatch 线程，慢回调不再阻塞 ring buffer 腾空 |
| **延迟监控** | 逐 session P50/P90/P99/P99.9 投递延迟直方图，`get_latency` / `reset_latency` |
| **状态接口** | 实时计数器：收发字节/消息数、发送 buffer 占用率 |
| **三种预设** | `LOW_FREQ` / `GENERAL` / `HIGH_THROUGHPUT`，开箱即用 |
| **纯 C API** | 不暴露任何 C++ 符号，可从 C / JNI / FFI 直接调用 |
| **小尺寸** | Android `.so` 约 420 KB（符号隐藏 + `-Os` + strip） |

---

## 目录结构

```
shmipc/
├── include/shmipc/
│   ├── shmipc.h          ← 唯一公开头文件（C API）
│   └── ShmConfig.h       ← 内部配置宏（install 时同步安装）
├── src/                  ← C++ 实现
├── examples/
│   ├── server_main.c     ← 示例：echo server
│   └── client_main.c     ← 示例：echo client
├── tests/
│   ├── test_common.h
│   ├── test1_s2c.c       ← Server→Client 基准
│   ├── test2_c2s.c       ← Client→Server 基准
│   ├── test3_duplex.c    ← 全双工 / 多线程 / 混合模式
│   ├── test4_zc.c        ← 零拷贝接收 API 验证
│   ├── test5_latency.c   ← 延迟监控 API 验证
│   └── README.md
├── CMakeLists.txt
└── install.sh            ← 一键打包 dist/
```

---

## 环境要求

| 组件 | 最低版本 | 说明 |
|------|----------|------|
| Linux 内核 | 4.14+ | `memfd_create`、`futex` |
| CMake | 3.14+ | |
| GCC / Clang | GCC 7+ / Clang 6+ | C++11 |
| Android NDK | r21+ | arm64-v8a 交叉编译 |

> 在 Windows 上请在 **WSL（Ubuntu 22.04）** 内构建。

---

## 构建

### 快速构建（本机 x86_64）

```bash
cd shmipc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

构建产物：
- `build/libshmipc.a` — 静态库
- `build/shmipc_server`、`build/shmipc_client` — 示例程序
- `build/shmipc_test1_s2c` … `build/shmipc_test5_latency` — 测试程序

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `SHMIPC_BUILD_SHARED` | `OFF` | `ON` 构建 `.so`，`OFF` 构建 `.a` |
| `SHMIPC_BUILD_EXAMPLES` | `ON` | 是否编译 examples/ |
| `SHMIPC_BUILD_TESTS` | `ON` | 是否编译 tests/ |

```bash
# 构建共享库，不编译示例和测试
cmake -S . -B build -DSHMIPC_BUILD_SHARED=ON \
      -DSHMIPC_BUILD_EXAMPLES=OFF -DSHMIPC_BUILD_TESTS=OFF
cmake --build build -j$(nproc)
```

### Android arm64-v8a 交叉编译

```bash
cmake -S shmipc -B build_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DSHMIPC_BUILD_SHARED=ON \
    -DSHMIPC_BUILD_EXAMPLES=OFF \
    -DSHMIPC_BUILD_TESTS=OFF
cmake --build build_arm64 -j$(nproc) --target shmipc
```

---

## 打包发布（install.sh）

```bash
# 在 shmipc/ 目录下执行（WSL 内）
bash install.sh                  # 共享库（默认）
bash install.sh --static         # 静态库
bash install.sh --skip-arm64     # 仅 x86_64
bash install.sh --skip-x86       # 仅 arm64-v8a
```

**环境变量：**

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `ANDROID_NDK_HOME` | `~/android-ndk-r28b` | NDK 根目录 |
| `DIST` | `./dist` | 输出目录 |
| `BUILD_TYPE` | `Release` | `Release` 或 `Debug` |

**输出结构：**

```
dist/
├── include/shmipc/
│   ├── shmipc.h
│   └── ShmConfig.h
└── lib/
    ├── x86_64/
    │   └── libshmipc.so   (~180 KB)
    └── arm64-v8a/
        └── libshmipc.so   (~420 KB)
```

---

## API 参考

### 配置预设

```c
#include "shmipc/shmipc.h"

SHMIPC_CONFIG_LOW_FREQ        // shm=8MB,  queue=32,  slice=4KB   — 低频控制消息
SHMIPC_CONFIG_GENERAL         // shm=16MB, queue=64,  slice=16KB  — 通用 IPC（默认）
SHMIPC_CONFIG_HIGH_THROUGHPUT // shm=64MB, queue=256, slice=64KB  — 高吞吐视频/音频

// 自定义配置
shmipc_config_t cfg = { .shm_size = 32u<<20, .event_queue_capacity = 128, .slice_size = 32768 };
```

**单次写入最大数据量：**

| 预设 | 最大单次写入 |
|------|------------|
| LOW_FREQ | ~4 MB |
| GENERAL | ~8 MB |
| HIGH_THROUGHPUT | ~32 MB |

### 写超时语义

| `timeout_ms` 值 | 宏 | 行为 |
|----------------|-----|------|
| `-1` | `SHMIPC_TIMEOUT_NONBLOCKING` | buffer 满立即返回，丢弃本次写入 |
| `0` | `SHMIPC_TIMEOUT_INFINITE` | 阻塞直到 buffer 有空间 |
| `N > 0` | — | 最多等待 N 毫秒，超时返回 `SHMIPC_TIMEOUT` |

### Server API

```c
shmipc_server_t* shmipc_server_create(void);
void             shmipc_server_destroy(shmipc_server_t*);

void shmipc_server_set_context(shmipc_server_t*, void* ctx);

// 注册回调
void shmipc_server_register_on_connected   (shmipc_server_t*, shmipc_on_session_cb);
void shmipc_server_register_on_data        (shmipc_server_t*, shmipc_on_data_cb);
void shmipc_server_register_on_data_zc     (shmipc_server_t*, shmipc_on_data_zc_cb);   // 零拷贝接收
void shmipc_server_register_on_disconnected(shmipc_server_t*, shmipc_on_disconnect_cb);

int  shmipc_server_start(shmipc_server_t*, const char* channel_name);
void shmipc_server_stop (shmipc_server_t*);

// 向指定 session 发送数据
int  shmipc_session_write(shmipc_session_t*, const void* data, uint32_t len, int32_t timeout_ms);

// 写端零拷贝（单 slice，len <= slice_size）
shmipc_wbuf_t* shmipc_session_alloc_buf   (shmipc_session_t*, uint32_t len);
int            shmipc_session_send_buf    (shmipc_session_t*, shmipc_wbuf_t*, uint32_t len);
void           shmipc_session_discard_buf (shmipc_session_t*, shmipc_wbuf_t*);

// 状态 & 延迟
void shmipc_server_get_status    (shmipc_server_t*,  shmipc_server_status_t*);
void shmipc_session_get_status   (shmipc_session_t*, shmipc_session_status_t*);
void shmipc_session_get_latency  (shmipc_session_t*, shmipc_latency_stats_t*);
void shmipc_session_reset_latency(shmipc_session_t*);

// 异步 dispatch（解耦慢回调与 ring buffer 消费）
void shmipc_server_set_async_dispatch(shmipc_server_t*, uint32_t queue_depth);
```

### Client API

```c
shmipc_client_t* shmipc_client_create(void);
void             shmipc_client_destroy(shmipc_client_t*);

void shmipc_client_set_context(shmipc_client_t*, void* ctx);
void shmipc_client_set_config (shmipc_client_t*, const shmipc_config_t*);

// 注册回调
void shmipc_client_register_on_connected   (shmipc_client_t*, shmipc_on_session_cb);
void shmipc_client_register_on_data        (shmipc_client_t*, shmipc_on_data_cb);
void shmipc_client_register_on_data_zc     (shmipc_client_t*, shmipc_cli_on_data_zc_cb);
void shmipc_client_register_on_disconnected(shmipc_client_t*, shmipc_on_disconnect_cb);

int  shmipc_client_connect   (shmipc_client_t*, const char* channel_name);
void shmipc_client_disconnect(shmipc_client_t*);

int  shmipc_client_write(shmipc_client_t*, const void* data, uint32_t len, int32_t timeout_ms);

// 写端零拷贝
shmipc_wbuf_t* shmipc_client_alloc_buf   (shmipc_client_t*, uint32_t len);
int            shmipc_client_send_buf    (shmipc_client_t*, shmipc_wbuf_t*, uint32_t len);
void           shmipc_client_discard_buf (shmipc_client_t*, shmipc_wbuf_t*);

// 状态 & 延迟
void shmipc_client_get_status  (shmipc_client_t*, shmipc_client_status_t*);
void shmipc_client_get_latency (shmipc_client_t*, shmipc_latency_stats_t*);
void shmipc_client_reset_latency(shmipc_client_t*);

// 异步 dispatch
void shmipc_client_set_async_dispatch(shmipc_client_t*, uint32_t queue_depth);
```

### 零拷贝接收缓冲区

```c
const void* shmipc_buf_data   (const shmipc_buf_t* buf);
uint32_t    shmipc_buf_len    (const shmipc_buf_t* buf);
void        shmipc_buf_release(shmipc_buf_t* buf);   // 必须恰好调用一次
```

---

## 快速上手

### Server（echo）

```c
#include "shmipc/shmipc.h"

static void on_data(shmipc_session_t* s, const void* data, uint32_t len, void* ctx) {
    shmipc_session_write(s, data, len, SHMIPC_TIMEOUT_INFINITE);
}

int main(void) {
    shmipc_server_t* srv = shmipc_server_create();
    shmipc_server_register_on_data(srv, on_data);

    shmipc_server_start(srv, "my_channel");
    pause();

    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}
```

### Client

```c
#include "shmipc/shmipc.h"

static void on_data(shmipc_session_t* s, const void* data, uint32_t len, void* ctx) {
    printf("recv %u bytes\n", len);
}

int main(void) {
    shmipc_client_t* cli = shmipc_client_create();
    shmipc_client_register_on_data(cli, on_data);

    shmipc_client_connect(cli, "my_channel");

    const char* msg = "hello";
    shmipc_client_write(cli, msg, 5, SHMIPC_TIMEOUT_INFINITE);

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}
```

### 零拷贝接收（`on_data_zc`）

```c
static void on_data_zc(shmipc_session_t* s, shmipc_buf_t* buf, void* ctx) {
    const void* data = shmipc_buf_data(buf);
    uint32_t    len  = shmipc_buf_len(buf);
    // 处理数据 ...
    shmipc_buf_release(buf);   // 必须调用
}

shmipc_server_register_on_data_zc(srv, on_data_zc);
```

### 写端零拷贝（`alloc_buf` / `send_buf`）

```c
// 单 slice 消息（len <= slice_size）免除内部 memcpy
shmipc_wbuf_t* wb = shmipc_session_alloc_buf(session, 1024);
if (wb) {
    memcpy(shmipc_wbuf_data(wb), my_data, 1024);
    shmipc_session_send_buf(session, wb, 1024);  // wb 已被消耗，不可再使用
}
```

### 延迟监控

```c
// 每 60 秒采集一次
shmipc_client_reset_latency(cli);
sleep(60);
shmipc_latency_stats_t st;
shmipc_client_get_latency(cli, &st);
printf("p50=%.1f µs  p99=%.1f µs  max=%.1f µs\n",
       st.p50_ns/1e3, st.p99_ns/1e3, st.max_ns/1e3);
```

### 异步 dispatch

```c
// 慢回调不再阻塞 ring buffer 消费
shmipc_server_set_async_dispatch(srv, 256);   // 队列深度 256 条，独立线程投递
shmipc_server_start(srv, "my_channel");
```

### 查询状态

```c
shmipc_client_status_t st;
shmipc_client_get_status(cli, &st);
printf("connected=%d  sent=%llu msgs  buf=%u%%\n",
       st.is_connected, (unsigned long long)st.msgs_sent, st.send_buffer_used_pct);
```

---

## 运行测试

```bash
cd shmipc/build
./shmipc_test1_s2c    2>/dev/null   # Server→Client 单向吞吐
./shmipc_test2_c2s    2>/dev/null   # Client→Server 单向吞吐
./shmipc_test3_duplex 2>/dev/null   # 全双工 + 多线程 + 混合模式
./shmipc_test4_zc     2>/dev/null   # 零拷贝接收 API 验证
./shmipc_test5_latency              # 延迟监控 API 验证
```

退出码 `0` = 全部 PASS，`1` = 有 FAIL。详细说明见 [`shmipc/tests/README.md`](shmipc/tests/README.md)。

### 性能参考（i5-12400，WSL2 Ubuntu 22.04）

| 场景 | 吞吐量 |
|------|--------|
| 单线程，1MB 负载，BLOCK | ~1.5 GB/s |
| 8 线程，1MB 负载，BLOCK | ~5.7 GB/s |
| 单线程，64KB 负载 | ~1.1 GB/s |
| 全双工，1MB ↔ 4KB，8 线程 | S→C ~6 GB/s，C→S ~0.4 GB/s |
| S→C 投递延迟（1KB，GENERAL） | p50 ≈ 1.5 µs，p99 ≈ 12 µs |

---

## 集成到 Android

### 使用预编译 `dist/`

```cmake
add_library(shmipc SHARED IMPORTED)
set_target_properties(shmipc PROPERTIES
    IMPORTED_LOCATION
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/lib/${ANDROID_ABI}/libshmipc.so"
    INTERFACE_INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/include"
)
target_link_libraries(my_native_lib PRIVATE shmipc)
```

---

## 注意事项

- **并发写是安全的**，但多线程竞争同一 session 在 NONBLK 模式下会导致丢弃。高并发场景建议每线程独立 session。
- **`on_data` / `on_data_zc` 在消费者线程执行**，不要做耗时操作；启用 `set_async_dispatch` 或拷贝后异步处理。
- **`on_connected` 触发后才可写**，不要在 `start` / `connect` 返回后立即写入。
- **`on_data_zc` 优先级高于 `on_data`**，两者同时注册时只调用前者。
- **`shmipc_buf_release` 必须恰好调用一次**，过多或遗漏均会导致内存泄漏或 SHM slice 耗尽。
- **`alloc_buf` / `send_buf` 始终消耗句柄**，调用后不可再使用该指针。
- `channel_name` 为 Unix Domain Socket 抽象命名空间路径，建议 ≤ 32 字符，仅含字母、数字、下划线。
