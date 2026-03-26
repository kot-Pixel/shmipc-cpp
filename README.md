# shmipc — 共享内存 IPC 库

基于共享内存（`memfd` + `mmap`）的高性能双向 IPC 框架，纯 C 公开 API，内部 C++11 实现，支持 Linux x86_64 与 Android arm64-v8a。

---

## 特性

- **零拷贝传输** — 数据直接写入 mmap 映射的共享内存，无 socket 拷贝
- **futex 通知** — 使用 `FUTEX_WAIT/WAKE` 替代 Unix Domain Socket 通知，减少上下文切换
- **全双工** — server_write 与 client_write 各自独立 ring buffer，双向并发无竞争
- **崩溃感知** — UDS 连接断开时自动触发 `on_disconnected` 回调并释放共享内存
- **背压控制** — 写入时支持阻塞、非阻塞、定时三种模式
- **三种预设** — LOW_FREQ / GENERAL / HIGH_THROUGHPUT，开箱即用
- **C API** — 不暴露任何 C++ 符号，可直接从 C / JNI / FFI 调用
- **小尺寸** — Android `.so` 约 420 KB（符号隐藏 + `-Os` + strip）

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
│   └── test3_duplex.c    ← 全双工 / 多线程 / 混合模式
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
cmake -S . -B build
cmake --build build -j4
```

构建产物：
- `build/libshmipc.a` — 静态库
- `build/shmipc_server`、`build/shmipc_client` — 示例程序（如开启）
- `build/shmipc_test1_s2c`、`build/shmipc_test2_c2s`、`build/shmipc_test3_duplex` — 测试程序

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `SHMIPC_BUILD_SHARED` | `OFF` | `ON` 构建 `.so`，`OFF` 构建 `.a` |
| `SHMIPC_BUILD_EXAMPLES` | `ON` | 是否编译 examples/ |
| `SHMIPC_BUILD_TESTS` | `ON` | 是否编译 tests/ |

```bash
# 构建共享库，不编译示例和测试
cmake -S . -B build -DSHMIPC_BUILD_SHARED=ON -DSHMIPC_BUILD_EXAMPLES=OFF -DSHMIPC_BUILD_TESTS=OFF
cmake --build build -j4
```

### 构建 Debug 版本

```bash
cmake -S . -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug -j4
```

---

## 打包发布（install.sh）

`install.sh` 一次性为 **x86_64** 和 **arm64-v8a** 构建库，并输出可直接集成的 `dist/` 目录。

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

// 直接使用内置预设（推荐）
SHMIPC_CONFIG_LOW_FREQ        // shm=8MB,  queue=32,  slice=4KB   — 低频控制消息
SHMIPC_CONFIG_GENERAL         // shm=16MB, queue=64,  slice=16KB  — 通用 IPC（默认）
SHMIPC_CONFIG_HIGH_THROUGHPUT // shm=64MB, queue=256, slice=64KB  — 高吞吐视频/音频

// 或自定义
shmipc_config_t cfg = { .shm_size = 32u<<20, .event_queue_capacity = 128, .slice_size = 32768 };
```

**单次写入最大数据量：**

| 预设 | 最大单次写入 |
|------|------------|
| LOW_FREQ | ~4 MB |
| GENERAL | ~8 MB |
| HIGH_THROUGHPUT | ~32 MB |

> 内部实现：`shm_size / 2` 为单侧 ring buffer，最大单次写约等于 buffer 总大小的一半。

---

### 写超时语义

| `timeout_ms` 值 | 宏 | 行为 |
|----------------|-----|------|
| `-1` | `SHMIPC_TIMEOUT_NONBLOCKING` | buffer 满立即返回，丢弃本次写入 |
| `0` | `SHMIPC_TIMEOUT_INFINITE` | 阻塞直到 buffer 有空间 |
| `N > 0` | — | 最多等待 N 毫秒，超时返回 `SHMIPC_TIMEOUT` |

---

### Server API

```c
// 创建 / 销毁
shmipc_server_t* shmipc_server_create(void);
void             shmipc_server_destroy(shmipc_server_t* server);

// 设置共享上下文（每个回调均可收到）
void shmipc_server_set_context(shmipc_server_t* server, void* ctx);

// 注册回调
void shmipc_server_register_on_connected   (shmipc_server_t*, shmipc_on_session_cb    cb);
void shmipc_server_register_on_data        (shmipc_server_t*, shmipc_on_data_cb        cb);
void shmipc_server_register_on_disconnected(shmipc_server_t*, shmipc_on_disconnect_cb  cb);

// 生命周期
int  shmipc_server_start(shmipc_server_t* server, const char* channel_name);
void shmipc_server_stop (shmipc_server_t* server);

// 向指定 session（客户端）发送数据
int shmipc_session_write(shmipc_session_t* session, const void* data, uint32_t len,
                         int32_t timeout_ms);

// 状态查询
void shmipc_server_get_status (shmipc_server_t*,  shmipc_server_status_t*  out);
void shmipc_session_get_status(shmipc_session_t*, shmipc_session_status_t* out);
```

---

### Client API

```c
// 创建 / 销毁
shmipc_client_t* shmipc_client_create(void);
void             shmipc_client_destroy(shmipc_client_t* client);

// 设置共享上下文
void shmipc_client_set_context(shmipc_client_t* client, void* ctx);

// 覆盖 channel 配置（必须在 connect 前调用；不调用则使用 GENERAL）
void shmipc_client_set_config(shmipc_client_t* client, const shmipc_config_t* config);

// 注册回调
void shmipc_client_register_on_connected   (shmipc_client_t*, shmipc_on_session_cb    cb);
void shmipc_client_register_on_data        (shmipc_client_t*, shmipc_on_data_cb        cb);
void shmipc_client_register_on_disconnected(shmipc_client_t*, shmipc_on_disconnect_cb  cb);

// 生命周期
int  shmipc_client_connect   (shmipc_client_t* client, const char* channel_name);
void shmipc_client_disconnect(shmipc_client_t* client);

// 向 server 发送数据
int shmipc_client_write(shmipc_client_t* client, const void* data, uint32_t len,
                        int32_t timeout_ms);

// 状态查询
void shmipc_client_get_status(shmipc_client_t* client, shmipc_client_status_t* out);
```

---

### 状态结构体

```c
// shmipc_server_get_status() 返回
typedef struct {
    int      is_running;        // 1 = 正在监听
    uint32_t connected_clients; // 当前连接数
} shmipc_server_status_t;

// shmipc_session_get_status() 返回（服务端针对某个客户端）
typedef struct {
    int      is_alive;              // 1 = 连接存活
    uint64_t bytes_sent;            // 服务端→客户端 字节数
    uint64_t msgs_sent;             // 服务端→客户端 消息数
    uint64_t bytes_received;        // 客户端→服务端 字节数
    uint64_t msgs_received;         // 客户端→服务端 消息数
    uint32_t send_buffer_used_pct;  // 服务端发送 buffer 占用率 (0-100)
} shmipc_session_status_t;

// shmipc_client_get_status() 返回
typedef struct {
    int      is_connected;
    uint64_t bytes_sent;
    uint64_t msgs_sent;
    uint64_t bytes_received;
    uint64_t msgs_received;
    uint32_t send_buffer_used_pct;  // 客户端发送 buffer 占用率 (0-100)
} shmipc_client_status_t;
```

---

## 快速上手

### Server 端

```c
#include "shmipc/shmipc.h"

typedef struct { int id; } MyCtx;

static void on_connected(shmipc_session_t* s, void* ctx) {
    printf("client connected\n");
}

static void on_data(shmipc_session_t* s, const void* data, uint32_t len, void* ctx) {
    // 收到数据后回写（echo）
    shmipc_session_write(s, data, len, SHMIPC_TIMEOUT_INFINITE);
}

static void on_disconnected(shmipc_session_t* s, void* ctx) {
    printf("client disconnected\n");
}

int main(void) {
    MyCtx ctx = { .id = 1 };

    shmipc_server_t* srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, on_connected);
    shmipc_server_register_on_data        (srv, on_data);
    shmipc_server_register_on_disconnected(srv, on_disconnected);

    if (shmipc_server_start(srv, "my_channel") != SHMIPC_OK) {
        shmipc_server_destroy(srv);
        return 1;
    }

    // 主循环...
    pause();

    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}
```

### Client 端

```c
#include "shmipc/shmipc.h"
#include <string.h>

static void on_data(shmipc_session_t* s, const void* data, uint32_t len, void* ctx) {
    printf("recv %u bytes\n", len);
}

int main(void) {
    shmipc_client_t* cli = shmipc_client_create();

    // 可选：指定非默认预设
    shmipc_client_set_config(cli, &SHMIPC_CONFIG_HIGH_THROUGHPUT);

    shmipc_client_register_on_data(cli, on_data);

    if (shmipc_client_connect(cli, "my_channel") != SHMIPC_OK) {
        shmipc_client_destroy(cli);
        return 1;
    }

    const char* msg = "hello";
    // 阻塞写（等待 buffer 有空间）
    shmipc_client_write(cli, msg, strlen(msg), SHMIPC_TIMEOUT_INFINITE);

    // 非阻塞写（buffer 满则立即丢弃）
    shmipc_client_write(cli, msg, strlen(msg), SHMIPC_TIMEOUT_NONBLOCKING);

    // 超时写（最多等 100ms）
    int rc = shmipc_client_write(cli, msg, strlen(msg), 100);
    if (rc == SHMIPC_TIMEOUT) { /* 超时处理 */ }

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}
```

### 查询状态

```c
shmipc_client_status_t st;
shmipc_client_get_status(cli, &st);
printf("connected=%d  sent=%llu msgs  buf=%u%%\n",
       st.is_connected, (unsigned long long)st.msgs_sent, st.send_buffer_used_pct);
```

---

## 集成到 Android 项目

### 使用 install.sh 打包

```bash
# 在 WSL 内
export ANDROID_NDK_HOME=~/android-ndk-r28b
bash install.sh          # 生成 dist/
```

### Android CMakeLists.txt

```cmake
# 导入预编译的 shmipc .so
add_library(shmipc SHARED IMPORTED)
set_target_properties(shmipc PROPERTIES
    IMPORTED_LOCATION
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/lib/${ANDROID_ABI}/libshmipc.so"
    INTERFACE_INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/include"
)

# 链接到你的目标
target_link_libraries(my_native_lib PRIVATE shmipc)
```

### 从源码交叉编译

```bash
cmake -S shmipc -B build_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DSHMIPC_BUILD_SHARED=ON \
    -DSHMIPC_BUILD_EXAMPLES=OFF \
    -DSHMIPC_BUILD_TESTS=OFF
cmake --build build_arm64 -j4 --target shmipc
```

---

## 运行测试

```bash
cd shmipc/build
./shmipc_test1_s2c  2>/dev/null   # Server→Client 单向
./shmipc_test2_c2s  2>/dev/null   # Client→Server 单向
./shmipc_test3_duplex 2>/dev/null  # 全双工 + 多线程 + 混合模式
```

退出码 `0` = 全部 PASS，`1` = 有 FAIL。详细测试说明见 [`tests/README.md`](tests/README.md)。

### 性能参考（i5-12400，WSL2）

| 场景 | 吞吐量 |
|------|--------|
| 单线程，1MB 负载，BLOCK | ~1.5 GB/s |
| 8 线程，1MB 负载，BLOCK | ~7 GB/s |
| 单线程，64KB 负载 | ~1.1 GB/s |
| 全双工，1MB ↔ 4KB，8 线程 | S→C ~6 GB/s，C→S ~0.4 GB/s |

---

## 注意事项

- **单进程内勿多线程并发写同一 session/client**：写锁在库内部，并发写是安全的，但多线程竞争同一写锁在 NONBLK 模式下会导致大量丢弃。
- **数据回调在专属线程内执行**，不要在回调中做耗时操作（建议拷贝数据后异步处理）。
- **`on_connected` 触发后 session 才可写**，不要在 `shmipc_server_start` 返回后立即写数据。
- **channel_name** 作为 Unix Domain Socket 路径前缀，长度建议 ≤ 32 字符，仅使用字母/数字/下划线。
- Android 上需确保 `/dev/shm` 可访问（或使用 `memfd_create`，已内置支持，API level 21+）。
