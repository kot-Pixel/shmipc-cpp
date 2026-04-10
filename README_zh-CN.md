**语言：** [English](README.md) | 简体中文

# shmipc — 共享内存 IPC 库

基于共享内存（`memfd` + `mmap`）的高性能双向 IPC 框架，纯 C 公开 API，内部 C++11 实现，支持 Linux x86_64 与 Android arm64-v8a。

> **一句话介绍：** 基于 futex 通知的零拷贝共享内存管道，提供简洁的 C 语言 API，专为 Linux 和 Android 上的高吞吐、低延迟本地 IPC 设计。
> **本仓库设计部分参考了**：https://github.com/cloudwego/shmipc-spec
---

## 特性

| 特性 | 说明 |
|------|------|
| **零拷贝接收** | 单 slice 消息直接借用 SHM 指针，`on_data_zc` 无堆拷贝 |
| **零拷贝写入** | `alloc_buf` / `send_buf` — 单条消息可单 slice 或多 slice，原地写 SHM，省去库内 `memcpy` |
| **futex 通知** | `FUTEX_WAIT/WAKE` 替代 UDS 数据通知，减少上下文切换 |
| **全双工** | `server_write` / `client_write` 独立 ring buffer，两侧并发无竞争 |
| **崩溃感知** | UDS 连接断开时自动触发 `on_disconnected` 并释放共享内存 |
| **背压控制** | 写入支持阻塞、非阻塞（丢弃）、定时三种模式 |
| **异步 dispatch** | 串行（保序）或并发（线程池，不保序）dispatch，慢回调不再阻塞 ring buffer 腾空 |
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
│   ├── test4_zc.c        ← 零拷贝接收 + 写端 alloc_buf/send_buf（含多 slice）
│   ├── test5_latency.c   ← 延迟监控 API 验证
│   ├── test7_dispatch.c  ← Dispatch：串行 + 并发线程池
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
- `build/shmipc_test1_s2c` … `build/shmipc_test7_dispatch` — 测试程序

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `SHMIPC_BUILD_SHARED` | `OFF` | `ON` 构建 `.so`，`OFF` 构建 `.a` |
| `SHMIPC_BUILD_EXAMPLES` | `ON` | 是否编译 examples/ |
| `SHMIPC_BUILD_TESTS` | `ON` | 是否编译 tests/ |
| `SHMIPC_ANDROID_MIN_SIZE` | `ON` | Android 静态库场景启用额外瘦身编译选项 |

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

### Android arm64-v8a：编译测试程序

与编译库相同，使用 NDK 工具链；保持 **`SHMIPC_BUILD_TESTS=ON`**（默认），并通常使用 **`SHMIPC_BUILD_SHARED=OFF`**，使每个测试可执行文件**静态链接** `libshmipc.a`，推送到真机时每个测试只需 `adb push` 一个文件。

在**仓库根目录**（`shmipc` 的上一级）执行：

```bash
export ANDROID_NDK_HOME=/path/to/ndk   # 例如 $HOME/android-ndk-r28b

cmake -S shmipc -B build_android_tests \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSHMIPC_BUILD_SHARED=OFF \
    -DSHMIPC_BUILD_EXAMPLES=OFF \
    -DSHMIPC_BUILD_TESTS=ON

cmake --build build_android_tests -j$(nproc)
```

若在 **`shmipc/`** 目录下操作，将 `-S shmipc` 改为 **`-S .`**，其余参数相同。

**产物：**

- `build_android_tests/libshmipc.a`
- `build_android_tests/shmipc_test1_s2c` … `build_android_tests/shmipc_test7_dispatch`（arm64 可执行文件）

**若使用共享库 + 测试：** 设置 `-DSHMIPC_BUILD_SHARED=ON`，先编 `shmipc` 再编各测试目标。在设备上运行时需把 **`libshmipc.so`** 与测试放在同一目录，或设置 **`LD_LIBRARY_PATH`** 指向该目录。

**在真机上运行（adb）：**

```bash
adb push build_android_tests/shmipc_test7_dispatch /data/local/tmp/
adb shell chmod 755 /data/local/tmp/shmipc_test7_dispatch
adb shell /data/local/tmp/shmipc_test7_dispatch
```

测试程序使用 `fork()`、管道与 `/dev/null`，与普通 Linux 上类似；适用于 **`adb shell` 环境**（将二进制推到 `/data/local/tmp/` 一般不需要 root）。若在极低 API 上链接或运行失败，可尝试 `-DANDROID_PLATFORM=android-24`。

### Android 静态库 `.a` 太大？（体积优先构建）

`.a` 是对象文件归档，保留符号与元数据，体积通常会明显大于 `.so`。  
若目标是尽量减小静态库体积，建议使用 `MinSizeRel`，并保持 `SHMIPC_ANDROID_MIN_SIZE=ON`（默认）：

```bash
cmake -S shmipc -B build_android_static_min \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DSHMIPC_BUILD_SHARED=OFF \
    -DSHMIPC_BUILD_EXAMPLES=OFF \
    -DSHMIPC_BUILD_TESTS=OFF \
    -DSHMIPC_ANDROID_MIN_SIZE=ON

cmake --build build_android_static_min -j$(nproc) --target shmipc
```

注意：
- 静态库与共享库请使用**不同的构建目录**（如 `build_android_static_min` / `build_arm64`），避免 CMake 缓存把 `SHMIPC_BUILD_SHARED` 沿用到下一次配置。
- `cmake --install` 默认安装到 `/usr/local`（普通用户可能无权限）。推荐：
  `cmake --install build_android_static_min --prefix ./dist_android_static_min`

若以最终 APK 体积为目标，仍推荐优先使用共享库方案（`SHMIPC_BUILD_SHARED=ON`），其链接裁剪与符号控制效果通常更好。

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

对外头文件仅 `#include "shmipc/shmipc.h"`。不透明类型：`shmipc_server_t`、`shmipc_client_t`、`shmipc_session_t`（服务端每个已连接客户端对应一个 session）、`shmipc_buf_t`（接收端零拷贝句柄）、`shmipc_wbuf_t`（写端零拷贝：一个或多个 SHM slice）。回调在库内部线程（消费者 / 可选 dispatch 线程）执行；若未启用异步 dispatch，应避免在回调中长时间阻塞。

### 返回值与宏

| 符号 | 值 | 含义 |
|------|-----|------|
| `SHMIPC_OK` | `0` | 成功 |
| `SHMIPC_ERR` | `-1` | 失败（参数非法、未连接、队列满等） |
| `SHMIPC_TIMEOUT` | `-2` | `timeout_ms > 0` 时等待超时 |

写接口的 `timeout_ms` 含义：

| `timeout_ms` | 宏 | 行为 |
|--------------|-----|------|
| `-1` | `SHMIPC_TIMEOUT_NONBLOCKING` | 发送侧 ring 满则立即丢弃本次写入 |
| `0` | `SHMIPC_TIMEOUT_INFINITE` | 阻塞直到有空间 |
| `N > 0` | — | 最多等待 N 毫秒，超时返回 `SHMIPC_TIMEOUT` |

### 配置

```c
typedef struct {
    uint32_t shm_size;             /* 共享内存总字节（对半分为 server_write / client_write） */
    uint32_t event_queue_capacity; /* 每方向事件环容量，≤ 512 */
    uint32_t slice_size;           /* 每个 slice 负载字节数 */
} shmipc_config_t;
```

**预设（只读全局量）：** `SHMIPC_CONFIG_LOW_FREQ`（8 MB / 32 / 4 KB）、`SHMIPC_CONFIG_GENERAL`（16 MB / 64 / 16 KB，客户端未调用 `set_config` 时的默认）、`SHMIPC_CONFIG_HIGH_THROUGHPUT`（64 MB / 256 / 64 KB）。

**谁设置配置：** 仅**客户端**在 `shmipc_client_connect` **之前**调用 `shmipc_client_set_config`。服务端使用握手协商出的参数，无需再调 `set_config`。

**单次写入最大 payload**（与内部 copy 写、多 slice 零拷贝共用同一几何上限，近似值）：

| 预设 | 最大单次写入 |
|------|------------|
| LOW_FREQ | ~4 MB |
| GENERAL | ~8 MB |
| HIGH_THROUGHPUT | ~32 MB |

### 回调类型

| 类型 | 调用时机 | 说明 |
|------|----------|------|
| `shmipc_on_session_cb` | 服务端：新客户端连上；客户端：`connect` 成功后的回调 | 服务端第一个参数为 `shmipc_session_t*`，用于后续向该客户端发送。 |
| `shmipc_on_data_cb` | 收到数据（拷贝路径） | `data` 仅在回调返回前有效。 |
| `shmipc_on_data_zc_cb` | 收到数据（服务端 session，零拷贝） | 若与 `on_data` 同时注册，仅调用本回调；必须 `shmipc_buf_release`。 |
| `shmipc_cli_on_data_zc_cb` | 收到数据（客户端，零拷贝） | 第一个参数为 `shmipc_client_t*`；必须 `shmipc_buf_release`。 |
| `shmipc_on_disconnect_cb` | 连接断开 | 释放与该句柄关联的应用状态。 |

### 服务端 API（`shmipc_server_*`）

| 函数 | 说明 |
|------|------|
| `shmipc_server_create()` | 创建服务端对象；失败返回 `NULL`。 |
| `shmipc_server_destroy(server)` | 停止监听并销毁会话；可传 `NULL`。 |
| `shmipc_server_set_context(server, ctx)` | 所有回调最后一个参数传入的上下文指针。 |
| `shmipc_server_register_on_connected(server, cb)` | 有客户端连接时调用；从回调取得 `shmipc_session_t*` 再发送。 |
| `shmipc_server_register_on_data(server, cb)` | **客户端→服务端** 接收（拷贝到用户态）。 |
| `shmipc_server_register_on_data_zc(server, cb)` | **客户端→服务端** 零拷贝接收；与 `on_data` 同时注册时优先本回调。 |
| `shmipc_server_register_on_disconnected(server, cb)` | 会话断开。 |
| `shmipc_server_start(server, channel_name)` | 在 UDS 抽象命名空间监听；成功返回 `SHMIPC_OK`。 |
| `shmipc_server_stop(server)` | 停止接受新连接并结束已有会话。 |
| `shmipc_server_get_status(server, out)` | `is_running`、`connected_clients`。 |
| `shmipc_server_set_async_dispatch(server, depth)` | `depth > 0` 时入队并由**单个 dispatch 线程**串行投递（保序）；须在 **`start` 之前** 调用；`0` 为同步投递（默认）。 |
| `shmipc_server_set_dispatch(server, depth, threads)` | 完整控制：`threads ≤ 1` → 串行（保序）；`threads > 1` → **并发线程池**（不保序，一条慢不阻塞其他）；须在 **`start` 之前** 调用。 |

### Session API（`shmipc_session_*`）——服务端向客户端发送

在 `on_connected` 拿到 `shmipc_session_t*` 后再写；**不要**在 `start` 返回后立刻写。

| 函数 | 说明 |
|------|------|
| `shmipc_session_write(session, data, len, timeout_ms)` | 将用户缓冲区拷贝进 SHM 并入队一条消息。 |
| `shmipc_session_get_status(session, out)` | 该会话收发字节/消息数、`send_buffer_used_pct`（**server_write** 方向环占用）。 |
| `shmipc_session_get_latency(session, out)` | **客户端→服务端** 方向接收延迟直方图（纳秒）；需已注册 `on_data` 或 `on_data_zc` 且会话有效。 |
| `shmipc_session_reset_latency(session)` | 清空该会话统计。 |

**写端零拷贝（`shmipc_wbuf_t`）：**

| 函数 | 说明 |
|------|------|
| `shmipc_session_alloc_buf(session, len)` | 为长度 `len` 的出站消息预留 SHM（不超过最大 payload）。`len ≤ slice_size` 时用单 slice，否则为 slice 链（与 `writData` 一致）。失败返回 `NULL`。 |
| `shmipc_session_send_buf(session, buf, len)` | 入队并**消耗** `buf`。**单 slice：** `len` ≤ `wbuf_capacity`（一般为 `slice_size`）。**多 slice：** `len` 须与 `alloc_buf` 时的 `len` 相同。 |
| `shmipc_session_discard_buf(session, buf)` | 不发送并释放 slice。 |

辅助：`shmipc_wbuf_data`（首段）、`shmipc_wbuf_capacity`、`shmipc_wbuf_num_slices`、`shmipc_wbuf_slice_data`、`shmipc_wbuf_slice_bytes`（`num_slices > 1` 时填各段）。

### 客户端 API（`shmipc_client_*`）

| 函数 | 说明 |
|------|------|
| `shmipc_client_create()` | 创建客户端对象。 |
| `shmipc_client_destroy(client)` | 断开并销毁。 |
| `shmipc_client_set_context(client, ctx)` | 回调上下文。 |
| `shmipc_client_set_config(client, config)` | 在 **`connect` 之前** 设置 SHM/队列/slice。 |
| `shmipc_client_register_on_connected(client, cb)` | 连接建立。 |
| `shmipc_client_register_on_data(client, cb)` | **服务端→客户端** 接收（拷贝）。 |
| `shmipc_client_register_on_data_zc(client, cb)` | **服务端→客户端** 零拷贝；第一个参数为 `shmipc_client_t*`。 |
| `shmipc_client_register_on_disconnected(client, cb)` | 断开。 |
| `shmipc_client_connect(client, channel_name)` | 连接；成功返回 `SHMIPC_OK`。 |
| `shmipc_client_disconnect(client)` | 主动断开。 |
| `shmipc_client_write(client, data, len, timeout_ms)` | **客户端→服务端** 拷贝写。 |
| `shmipc_client_get_status(client, out)` | 含 **client_write** 方向 `send_buffer_used_pct`。 |
| `shmipc_client_get_latency(client, out)` | **服务端→客户端** 接收延迟直方图。 |
| `shmipc_client_reset_latency(client)` | 清空统计。 |
| `shmipc_client_set_async_dispatch(client, depth)` | 与服务端 `set_async_dispatch` 相同；须在 **`connect` 之前** 调用。 |
| `shmipc_client_set_dispatch(client, depth, threads)` | 与服务端 `set_dispatch` 相同；须在 **`connect` 之前** 调用。 |

**写端零拷贝（客户端）：** `shmipc_client_alloc_buf` / `send_buf` / `discard_buf`，与上表 session 侧语义一致。

### 零拷贝接收缓冲区（`shmipc_buf_t`）

仅在 `on_data_zc` / `shmipc_cli_on_data_zc_cb` 内使用：

| 函数 | 说明 |
|------|------|
| `shmipc_buf_data(buf)` | 数据指针（单 slice 时可能直接指向 SHM）。 |
| `shmipc_buf_len(buf)` | 长度。 |
| `shmipc_buf_release(buf)` | **每条** 收到的 buffer **必须且仅能** 调用一次。 |

### 状态结构体

- **`shmipc_server_status_t`：** `is_running`、`connected_clients`。
- **`shmipc_session_status_t`：** `is_alive`、该会话收发统计、`send_buffer_used_pct`（服务端→客户端发送环）。
- **`shmipc_client_status_t`：** `is_connected`、收发统计、`send_buffer_used_pct`（客户端→服务端发送环）。

### 延迟统计（`shmipc_latency_stats_t`）

字段（纳秒）：`count`、`min_ns`、`avg_ns`、`p50_ns`、`p90_ns`、`p99_ns`、`p999_ns`、`max_ns`。分位数为近似值（log₂ 分桶）。`count == 0` 表示尚无样本。

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

### 多 slice 写端零拷贝（仍用 `alloc_buf` / `send_buf`）

```c
uint32_t total = 100000;
shmipc_wbuf_t* wb = shmipc_session_alloc_buf(session, total);
if (wb) {
    for (uint32_t i = 0; i < shmipc_wbuf_num_slices(wb); i++) {
        void*    seg = shmipc_wbuf_slice_data(wb, i);
        uint32_t n   = shmipc_wbuf_slice_bytes(wb, i);
        fill_segment(seg, n);
    }
    shmipc_session_send_buf(session, wb, total);
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

### Dispatch 模式

```c
// 串行 dispatch：保持消息顺序（单 dispatch 线程）
shmipc_server_set_async_dispatch(srv, 256);    // 旧 API，等价于 set_dispatch(256, 1)
shmipc_server_start(srv, "my_channel");

// 并发 dispatch：线程池，不保序，一条慢回调不阻塞其他
shmipc_client_set_dispatch(cli, 128, 4);       // 队列深度 128，4 个 dispatch 线程
shmipc_client_connect(cli, "my_channel");
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
./shmipc_test4_zc     2>/dev/null   # 零拷贝接收 + 写端 alloc_buf（C→S / S→C）
./shmipc_test5_latency              # 延迟监控 API 验证
./shmipc_test7_dispatch             # Dispatch：串行 + 并发（S→C 与 C→S）
```

退出码 `0` = 全部 PASS，`1` = 有 FAIL。详细说明见 [`shmipc/tests/README.md`](shmipc/tests/README.md)。

**Android 真机：** 使用 NDK 交叉编译上述测试可执行文件，并用 `adb push` / `adb shell` 运行；步骤见上文 **「Android arm64-v8a：编译测试程序」** 一节。

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
- **`on_data` / `on_data_zc` 默认在消费者线程执行**，不要做耗时操作；启用 `set_dispatch`（串行或并发）或拷贝后异步处理。
- **`on_connected` 触发后才可写**，不要在 `start` / `connect` 返回后立即写入。
- **`on_data_zc` 优先级高于 `on_data`**，两者同时注册时只调用前者。
- **`shmipc_buf_release` 必须恰好调用一次**，过多或遗漏均会导致内存泄漏或 SHM slice 耗尽。
- **`alloc_buf` / `send_buf` 始终消耗句柄**，调用后不可再使用该指针。
- **多 slice 时 `send_buf` 的 `len` 须与 `alloc_buf` 时的 `len` 一致**。
- `channel_name` 为 Unix Domain Socket 抽象命名空间路径，建议 ≤ 32 字符，仅含字母、数字、下划线。
