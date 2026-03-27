**Language:** English | [简体中文](README_zh-CN.md)

# shmipc — Shared Memory IPC Library

A high-performance, bidirectional IPC framework built on shared memory (`memfd` + `mmap`). Pure C public API, C++11 internals, supports Linux x86_64 and Android arm64-v8a.

> **One-sentence pitch:** A futex-driven, zero-copy shared-memory pipe with a clean C API, designed for high-throughput, low-latency local IPC on Linux and Android.

---

## Features

| Feature | Description |
|---------|-------------|
| **Zero-copy receive** | Single-slice messages borrow the SHM pointer directly — no heap copy in `on_data_zc` |
| **Zero-copy write** | `alloc_buf` / `send_buf` lets callers fill SHM slices in-place, skipping the internal `memcpy` |
| **futex notification** | `FUTEX_WAIT/WAKE` replaces Unix Domain Socket data signals, reducing context switches |
| **Full-duplex** | Independent `server_write` / `client_write` ring buffers — no contention in either direction |
| **Crash awareness** | UDS socket closure triggers `on_disconnected` and shared-memory cleanup automatically |
| **Back-pressure** | Three write modes: blocking, non-blocking (drop), timed (up to N ms) |
| **Async dispatch** | Optional decoupled dispatch thread so slow `on_data` callbacks never stall ring-buffer draining |
| **Latency monitoring** | Per-session P50/P90/P99/P99.9 delivery-latency histograms with `get_latency` / `reset_latency` |
| **Status API** | Real-time counters: bytes/messages sent & received, send-buffer fullness % |
| **Three presets** | `LOW_FREQ` / `GENERAL` / `HIGH_THROUGHPUT` — ready to use out of the box |
| **Pure C API** | No C++ symbols exposed; callable from C, JNI, FFI |
| **Small footprint** | Android `.so` ≈ 420 KB (hidden symbols + `-Os` + strip) |

---

## Directory Layout

```
shmipc/
├── include/shmipc/
│   ├── shmipc.h          ← sole public header (C API)
│   └── ShmConfig.h       ← internal config macros (installed alongside)
├── src/                  ← C++ implementation
├── examples/
│   ├── server_main.c
│   └── client_main.c
├── tests/
│   ├── test_common.h
│   ├── test1_s2c.c       ← Server→Client benchmark
│   ├── test2_c2s.c       ← Client→Server benchmark
│   ├── test3_duplex.c    ← Full-duplex / multi-thread / mixed modes
│   ├── test4_zc.c        ← Zero-copy receive API validation
│   ├── test5_latency.c   ← Latency monitoring API validation
│   └── README.md
├── CMakeLists.txt
└── install.sh            ← one-shot dist/ packager
```

---

## Requirements

| Component | Minimum | Notes |
|-----------|---------|-------|
| Linux kernel | 4.14+ | `memfd_create`, `futex` |
| CMake | 3.14+ | |
| GCC / Clang | GCC 7+ / Clang 6+ | C++11 |
| Android NDK | r21+ | arm64-v8a cross-compilation |

> On Windows, build inside **WSL (Ubuntu 22.04)**.

---

## Building

### Quick build (host x86_64)

```bash
cd shmipc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Outputs:
- `build/libshmipc.a` — static library
- `build/shmipc_server`, `build/shmipc_client` — example binaries
- `build/shmipc_test1_s2c` … `build/shmipc_test5_latency` — test binaries

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `SHMIPC_BUILD_SHARED` | `OFF` | `ON` builds `.so`, `OFF` builds `.a` |
| `SHMIPC_BUILD_EXAMPLES` | `ON` | Build examples/ |
| `SHMIPC_BUILD_TESTS` | `ON` | Build tests/ |
| `SHMIPC_ANDROID_MIN_SIZE` | `ON` | Extra Android size flags for static workflows |

```bash
# Shared library only, no examples or tests
cmake -S . -B build -DSHMIPC_BUILD_SHARED=ON \
      -DSHMIPC_BUILD_EXAMPLES=OFF -DSHMIPC_BUILD_TESTS=OFF
cmake --build build -j$(nproc)
```

### Android arm64-v8a cross-compilation

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

### Android arm64-v8a: test binaries

Cross-compile tests with the NDK; keep **`SHMIPC_BUILD_TESTS=ON`** and usually **`SHMIPC_BUILD_SHARED=OFF`** so each test statically links `libshmipc.a` (one file per `adb push`).

From **`shmipc/`**:

```bash
export ANDROID_NDK_HOME=/path/to/ndk

cmake -S . -B build_android_tests \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSHMIPC_BUILD_SHARED=OFF \
    -DSHMIPC_BUILD_EXAMPLES=OFF \
    -DSHMIPC_BUILD_TESTS=ON

cmake --build build_android_tests -j$(nproc)
```

**Outputs:** `build_android_tests/libshmipc.a`, `build_android_tests/shmipc_test1_s2c`, …, `shmipc_test7_dispatch`.

**Run on device:**

```bash
adb push build_android_tests/shmipc_test7_dispatch /data/local/tmp/
adb shell chmod 755 /data/local/tmp/shmipc_test7_dispatch
adb shell /data/local/tmp/shmipc_test7_dispatch
```

If you use **`SHMIPC_BUILD_SHARED=ON`**, push `libshmipc.so` next to the binary or set **`LD_LIBRARY_PATH`**. Try **`ANDROID_PLATFORM=android-24`** if you hit link/runtime issues on old APIs.

### Android static `.a` too large? (size-first build)

`.a` archives keep object files and symbol metadata, so they are usually much larger than `.so`.
For minimum static size, use `MinSizeRel` + section flags (`SHMIPC_ANDROID_MIN_SIZE=ON`):

```bash
cmake -S . -B build_android_static_min \
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

Important:
- Use a **new build directory** when switching static/shared builds to avoid CMake cache reusing `SHMIPC_BUILD_SHARED`.
- `cmake --install` defaults to `/usr/local` and may fail without permission. Use:
  `cmake --install build_android_static_min --prefix ./dist_android_static_min`

---

## Packaging (`install.sh`)

Builds for both x86_64 and arm64-v8a and produces an integration-ready `dist/` tree.

```bash
# Run inside shmipc/ (WSL)
bash install.sh                  # shared library (default)
bash install.sh --static         # static library
bash install.sh --skip-arm64     # x86_64 only
bash install.sh --skip-x86       # arm64-v8a only
```

**Environment variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `ANDROID_NDK_HOME` | `~/android-ndk-r28b` | NDK root |
| `DIST` | `./dist` | Output directory |
| `BUILD_TYPE` | `Release` | `Release` or `Debug` |

**Output layout:**

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

## API Reference

### Configuration presets

```c
#include "shmipc/shmipc.h"

SHMIPC_CONFIG_LOW_FREQ        // shm=8MB,  queue=32,  slice=4KB   — low-frequency control
SHMIPC_CONFIG_GENERAL         // shm=16MB, queue=64,  slice=16KB  — general IPC (default)
SHMIPC_CONFIG_HIGH_THROUGHPUT // shm=64MB, queue=256, slice=64KB  — video / audio streams

// Custom config
shmipc_config_t cfg = { .shm_size = 32u<<20, .event_queue_capacity = 128, .slice_size = 32768 };
```

**Maximum single-write payload:**

| Preset | Max payload |
|--------|-------------|
| LOW_FREQ | ~4 MB |
| GENERAL | ~8 MB |
| HIGH_THROUGHPUT | ~32 MB |

### Write timeout semantics

| `timeout_ms` | Macro | Behaviour |
|-------------|-------|-----------|
| `-1` | `SHMIPC_TIMEOUT_NONBLOCKING` | Drop immediately if buffer full |
| `0` | `SHMIPC_TIMEOUT_INFINITE` | Block until space is available |
| `N > 0` | — | Wait at most N ms; returns `SHMIPC_TIMEOUT` |

### Server API

```c
shmipc_server_t* shmipc_server_create(void);
void             shmipc_server_destroy(shmipc_server_t* server);

void shmipc_server_set_context(shmipc_server_t*, void* ctx);

// Callbacks
void shmipc_server_register_on_connected   (shmipc_server_t*, shmipc_on_session_cb);
void shmipc_server_register_on_data        (shmipc_server_t*, shmipc_on_data_cb);
void shmipc_server_register_on_data_zc     (shmipc_server_t*, shmipc_on_data_zc_cb);   // zero-copy receive
void shmipc_server_register_on_disconnected(shmipc_server_t*, shmipc_on_disconnect_cb);

int  shmipc_server_start(shmipc_server_t*, const char* channel_name);
void shmipc_server_stop (shmipc_server_t*);

// Write to a specific client
int  shmipc_session_write(shmipc_session_t*, const void* data, uint32_t len, int32_t timeout_ms);

// Write-side zero-copy (single-slice, len <= slice_size)
shmipc_wbuf_t* shmipc_session_alloc_buf   (shmipc_session_t*, uint32_t len);
int            shmipc_session_send_buf    (shmipc_session_t*, shmipc_wbuf_t*, uint32_t len);
void           shmipc_session_discard_buf (shmipc_session_t*, shmipc_wbuf_t*);

// Status & latency
void shmipc_server_get_status  (shmipc_server_t*,  shmipc_server_status_t*);
void shmipc_session_get_status (shmipc_session_t*, shmipc_session_status_t*);
void shmipc_session_get_latency(shmipc_session_t*, shmipc_latency_stats_t*);
void shmipc_session_reset_latency(shmipc_session_t*);

// Async dispatch (decouple slow callbacks from ring-buffer draining)
void shmipc_server_set_async_dispatch(shmipc_server_t*, uint32_t queue_depth);
```

### Client API

```c
shmipc_client_t* shmipc_client_create(void);
void             shmipc_client_destroy(shmipc_client_t*);

void shmipc_client_set_context(shmipc_client_t*, void* ctx);
void shmipc_client_set_config (shmipc_client_t*, const shmipc_config_t*);

// Callbacks
void shmipc_client_register_on_connected   (shmipc_client_t*, shmipc_on_session_cb);
void shmipc_client_register_on_data        (shmipc_client_t*, shmipc_on_data_cb);
void shmipc_client_register_on_data_zc     (shmipc_client_t*, shmipc_cli_on_data_zc_cb);
void shmipc_client_register_on_disconnected(shmipc_client_t*, shmipc_on_disconnect_cb);

int  shmipc_client_connect   (shmipc_client_t*, const char* channel_name);
void shmipc_client_disconnect(shmipc_client_t*);

int  shmipc_client_write(shmipc_client_t*, const void* data, uint32_t len, int32_t timeout_ms);

// Write-side zero-copy
shmipc_wbuf_t* shmipc_client_alloc_buf   (shmipc_client_t*, uint32_t len);
int            shmipc_client_send_buf    (shmipc_client_t*, shmipc_wbuf_t*, uint32_t len);
void           shmipc_client_discard_buf (shmipc_client_t*, shmipc_wbuf_t*);

// Status & latency
void shmipc_client_get_status  (shmipc_client_t*, shmipc_client_status_t*);
void shmipc_client_get_latency (shmipc_client_t*, shmipc_latency_stats_t*);
void shmipc_client_reset_latency(shmipc_client_t*);

// Async dispatch
void shmipc_client_set_async_dispatch(shmipc_client_t*, uint32_t queue_depth);
```

### Zero-copy receive buffer

```c
const void* shmipc_buf_data   (const shmipc_buf_t* buf);
uint32_t    shmipc_buf_len    (const shmipc_buf_t* buf);
void        shmipc_buf_release(shmipc_buf_t* buf);   // MUST be called exactly once
```

### Latency stats

```c
typedef struct {
    uint64_t count;    // total messages sampled
    uint64_t min_ns;   // minimum latency (ns)
    uint64_t avg_ns;   // mean latency (ns)
    uint64_t p50_ns;   // 50th percentile (ns)
    uint64_t p90_ns;   // 90th percentile (ns)
    uint64_t p99_ns;   // 99th percentile (ns)
    uint64_t p999_ns;  // 99.9th percentile (ns)
    uint64_t max_ns;   // maximum latency (ns)
} shmipc_latency_stats_t;
```

---

## Quick-Start Examples

### Server (echo)

```c
#include "shmipc/shmipc.h"

static void on_data(shmipc_session_t* s, const void* data, uint32_t len, void* ctx) {
    shmipc_session_write(s, data, len, SHMIPC_TIMEOUT_INFINITE);
}

int main(void) {
    shmipc_server_t* srv = shmipc_server_create();
    shmipc_server_register_on_data(srv, on_data);

    shmipc_server_start(srv, "my_channel");
    pause();  // run forever

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
    shmipc_client_write(cli, msg, 5, SHMIPC_TIMEOUT_INFINITE);  // blocking write

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}
```

### Zero-copy receive (`on_data_zc`)

```c
static void on_data_zc(shmipc_session_t* s, shmipc_buf_t* buf, void* ctx) {
    const void* data = shmipc_buf_data(buf);
    uint32_t    len  = shmipc_buf_len(buf);
    // process data ...
    shmipc_buf_release(buf);   // MUST be called
}

shmipc_server_register_on_data_zc(srv, on_data_zc);
```

### Write-side zero-copy (`alloc_buf` / `send_buf`)

```c
// Avoids the internal memcpy for single-slice messages (len <= slice_size)
shmipc_wbuf_t* wb = shmipc_session_alloc_buf(session, 1024);
if (wb) {
    memcpy(shmipc_wbuf_data(wb), my_data, 1024);
    shmipc_session_send_buf(session, wb, 1024);  // wb is consumed; do not use after
}
```

### Latency monitoring

```c
// Periodic monitoring window
shmipc_client_reset_latency(cli);
sleep(60);
shmipc_latency_stats_t st;
shmipc_client_get_latency(cli, &st);
printf("p50=%.1f µs  p99=%.1f µs  max=%.1f µs\n",
       st.p50_ns/1e3, st.p99_ns/1e3, st.max_ns/1e3);
```

### Async dispatch

```c
// Prevent slow on_data from stalling the ring-buffer consumer
shmipc_server_set_async_dispatch(srv, 256);   // 256-slot queue, dedicated thread
shmipc_server_start(srv, "my_channel");
```

---

## Running Tests

```bash
cd shmipc/build
./shmipc_test1_s2c    2>/dev/null   # Server→Client throughput
./shmipc_test2_c2s    2>/dev/null   # Client→Server throughput
./shmipc_test3_duplex 2>/dev/null   # Full-duplex + multi-thread + mixed modes
./shmipc_test4_zc     2>/dev/null   # Zero-copy receive API
./shmipc_test5_latency              # Latency monitoring API
```

Exit code `0` = all PASS, `1` = failure. See [`shmipc/tests/README.md`](shmipc/tests/README.md) for detailed descriptions.

### Performance reference (i5-12400, WSL2 Ubuntu 22.04)

| Scenario | Throughput |
|----------|-----------|
| 1 thread, 1 MB payload, BLOCK | ~1.5 GB/s |
| 8 threads, 1 MB payload, BLOCK | ~5.7 GB/s |
| 1 thread, 64 KB payload | ~1.1 GB/s |
| Full-duplex, 1 MB ↔ 4 KB, 8 threads | S→C ~6 GB/s, C→S ~0.4 GB/s |
| S→C delivery latency (1 KB, GENERAL) | p50 ≈ 1.5 µs, p99 ≈ 12 µs |

---

## Android Integration

### Using the prebuilt `dist/`

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

## Notes

- **Concurrent writes are safe**, but NONBLOCKING writers on the same session may drop messages under contention. Use per-thread sessions for the best throughput.
- **`on_data` / `on_data_zc` run on the consumer thread.** For slow processing, enable `set_async_dispatch` or copy data and process asynchronously.
- **`on_connected` must fire before writing** — do not write inside `shmipc_server_start` / `shmipc_client_connect`; wait for the callback.
- **`on_data_zc` takes priority** over `on_data` when both are registered.
- **`shmipc_buf_release` must be called exactly once** for every `shmipc_buf_t*` received via `on_data_zc`.
- **`alloc_buf` / `send_buf` always consume the handle** — never use the pointer after calling either function.
- `channel_name` is a Unix Domain Socket abstract namespace path. Keep it ≤ 32 characters (letters, digits, underscores).
