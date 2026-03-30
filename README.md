**Language:** English | [简体中文](README_zh-CN.md)

# shmipc — Shared Memory IPC Library

A high-performance, bidirectional IPC framework built on shared memory (`memfd` + `mmap`). Pure C public API, C++11 internals, supports Linux x86_64 and Android arm64-v8a.

> **One-sentence pitch:** A futex-driven, zero-copy shared-memory pipe with a clean C API, designed for high-throughput, low-latency local IPC on Linux and Android.

---

## Features

| Feature | Description |
|---------|-------------|
| **Zero-copy receive** | Single-slice messages borrow the SHM pointer directly — no heap copy in `on_data_zc` |
| **Zero-copy write** | `alloc_buf` / `send_buf` — one or many slices per message; fill SHM in-place, skipping internal `memcpy` |
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
│   ├── test4_zc.c        ← Zero-copy receive + write alloc_buf/send_buf (incl. multi-slice)
│   ├── test5_latency.c   ← Latency monitoring API validation
│   ├── test7_dispatch.c  ← Async dispatch (slow callback + burst)
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
- `build/shmipc_test1_s2c` … `build/shmipc_test7_dispatch` — test binaries

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

Public API lives in a single header: `#include "shmipc/shmipc.h"`. Opaque handles: `shmipc_server_t`, `shmipc_client_t`, `shmipc_session_t` (per connected client on the server), `shmipc_buf_t` (receive, zero-copy path), `shmipc_wbuf_t` (write zero-copy buffer: one or more SHM slices). Callbacks run on internal library threads (consumer / optional dispatch); do not block for long unless you use async dispatch.

### Return codes and macros

| Symbol | Value | Meaning |
|--------|-------|---------|
| `SHMIPC_OK` | `0` | Success |
| `SHMIPC_ERR` | `-1` | Failure (invalid argument, not connected, queue full, etc.) |
| `SHMIPC_TIMEOUT` | `-2` | Timed wait expired (`timeout_ms` > 0) |

Write APIs accept `timeout_ms` using:

| `timeout_ms` | Macro | Behaviour |
|--------------|-------|-----------|
| `-1` | `SHMIPC_TIMEOUT_NONBLOCKING` | Drop immediately if the outbound ring is full |
| `0` | `SHMIPC_TIMEOUT_INFINITE` | Block until space is available |
| `N > 0` | — | Wait at most N ms; on timeout return `SHMIPC_TIMEOUT` |

### Configuration

```c
typedef struct {
    uint32_t shm_size;             /* total SHM bytes (split: server_write | client_write) */
    uint32_t event_queue_capacity; /* ring slots per direction, ≤ 512 */
    uint32_t slice_size;           /* payload bytes per slice */
} shmipc_config_t;
```

**Presets (read-only globals):** `SHMIPC_CONFIG_LOW_FREQ` (8 MB / 32 / 4 KB), `SHMIPC_CONFIG_GENERAL` (16 MB / 64 / 16 KB, default if client does not call `set_config`), `SHMIPC_CONFIG_HIGH_THROUGHPUT` (64 MB / 256 / 64 KB).

**Who sets config:** Only the **client** calls `shmipc_client_set_config` **before** `shmipc_client_connect`. The server accepts the negotiated metadata from the client; it does not mirror `set_config`.

**Maximum single-write payload** (approximate upper bound from region geometry; same for copy-write and multi-slice zero-copy):

| Preset | Max payload |
|--------|-------------|
| LOW_FREQ | ~4 MB |
| GENERAL | ~8 MB |
| HIGH_THROUGHPUT | ~32 MB |

### Callback types

| Callback | When invoked | Notes |
|----------|----------------|-------|
| `shmipc_on_session_cb` | Client connected (server) or your client finished connect (client) | First argument is `shmipc_session_t*` (server) or passed as session for client-side registration; second is `ctx`. |
| `shmipc_on_data_cb` | Inbound message (copying path) | `data` is valid only for the duration of the callback. |
| `shmipc_on_data_zc_cb` | Inbound message (server session, zero-copy) | Prefer over `on_data` if both set. You must `shmipc_buf_release(buf)`. |
| `shmipc_cli_on_data_zc_cb` | Inbound message (client, zero-copy) | Same release rule. |
| `shmipc_on_disconnect_cb` | Session or client disconnected | Tear down app state tied to the handle. |

### Server API (`shmipc_server_*`)

| Function | Purpose |
|----------|---------|
| `shmipc_server_create()` | Allocate a server object. Returns non-NULL or `NULL` on OOM. |
| `shmipc_server_destroy(server)` | Stops listening, destroys sessions, frees object. Safe on `NULL`. |
| `shmipc_server_set_context(server, ctx)` | Opaque pointer passed as last argument to every callback. |
| `shmipc_server_register_on_connected(server, cb)` | Called when a client connects; use `shmipc_session_t*` from `cb` for later writes. |
| `shmipc_server_register_on_data(server, cb)` | Copying receive path for **client → server** data. |
| `shmipc_server_register_on_data_zc(server, cb)` | Zero-copy receive; overrides `on_data` if both registered. |
| `shmipc_server_register_on_disconnected(server, cb)` | Called when a session ends. |
| `shmipc_server_start(server, channel_name)` | Listen on UDS abstract name; returns `SHMIPC_OK` or `SHMIPC_ERR`. |
| `shmipc_server_stop(server)` | Stop accepting; existing sessions are torn down. |
| `shmipc_server_get_status(server, out)` | Snapshot: `is_running`, `connected_clients`. |
| `shmipc_server_set_async_dispatch(server, depth)` | If `depth > 0`, incoming messages are queued and delivered on a **dispatch thread**. Must be called **before** `start`. `0` = synchronous (default). |

### Session API (`shmipc_session_*`) — server → client send

Obtain `shmipc_session_t*` from `on_connected`. Do **not** write before that callback fires.

| Function | Purpose |
|----------|---------|
| `shmipc_session_write(session, data, len, timeout_ms)` | Copy `data` into shared memory and enqueue one message. |
| `shmipc_session_get_status(session, out)` | Per-session bytes/messages sent and received, `send_buffer_used_pct` for **server_write** ring. |
| `shmipc_session_get_latency(session, out)` | Histogram for **client → server** receive latency (nanoseconds). Requires `on_data` or `on_data_zc` registered and an active session. |
| `shmipc_session_reset_latency(session)` | Clears samples for that session. |

**Write-side zero-copy (`shmipc_wbuf_t`):**

| Function | Purpose |
|----------|---------|
| `shmipc_session_alloc_buf(session, len)` | Reserves SHM for an outbound message of `len` bytes (within max payload). Uses **one slice** if `len ≤ slice_size`, otherwise a **slice chain** (same layout as `writData`). Returns `NULL` on failure. |
| `shmipc_session_send_buf(session, buf, len)` | Enqueues the message and **frees** `buf`. **Single-slice:** `len` ≤ `wbuf_capacity` (typically `slice_size`). **Multi-slice:** `len` must equal the `len` passed to `alloc_buf`. Returns `SHMIPC_OK` or `SHMIPC_ERR`. |
| `shmipc_session_discard_buf(session, buf)` | Frees slice(s) without sending. |

Helpers: `shmipc_wbuf_data(buf)` → first segment; `shmipc_wbuf_capacity(buf)` → max bytes for `send_buf`; `shmipc_wbuf_num_slices`, `shmipc_wbuf_slice_data`, `shmipc_wbuf_slice_bytes` for segment `i` when `num_slices > 1`.

### Client API (`shmipc_client_*`)

| Function | Purpose |
|----------|---------|
| `shmipc_client_create()` | Allocate client object. |
| `shmipc_client_destroy(client)` | Disconnects if needed, frees object. |
| `shmipc_client_set_context(client, ctx)` | Opaque pointer for callbacks. |
| `shmipc_client_set_config(client, config)` | **Before** `connect`. Chooses SHM size, queue capacity, slice size. |
| `shmipc_client_register_on_connected(client, cb)` | Called when connection is up. |
| `shmipc_client_register_on_data(client, cb)` | Copying receive for **server → client** data. |
| `shmipc_client_register_on_data_zc(client, cb)` | Zero-copy receive; first argument is `shmipc_client_t*`. |
| `shmipc_client_register_on_disconnected(client, cb)` | Called on disconnect. |
| `shmipc_client_connect(client, channel_name)` | Returns `SHMIPC_OK` on success. |
| `shmipc_client_disconnect(client)` | Closes session. |
| `shmipc_client_write(client, data, len, timeout_ms)` | Client → server copy-write. |
| `shmipc_client_get_status(client, out)` | Snapshot including `send_buffer_used_pct` for **client_write** ring. |
| `shmipc_client_get_latency(client, out)` | Histogram for **server → client** receive latency. |
| `shmipc_client_reset_latency(client)` | Clears samples. |
| `shmipc_client_set_async_dispatch(client, depth)` | Same semantics as server; call **before** `connect`. |

**Write-side zero-copy (client):** `shmipc_client_alloc_buf`, `shmipc_client_send_buf`, `shmipc_client_discard_buf` — same semantics as the session functions above.

### Zero-copy receive buffer (`shmipc_buf_t`)

Used only inside `on_data_zc` / `shmipc_cli_on_data_zc_cb`:

| Function | Purpose |
|----------|---------|
| `shmipc_buf_data(buf)` | Pointer to payload (may be in SHM for single-slice messages). |
| `shmipc_buf_len(buf)` | Byte length. |
| `shmipc_buf_release(buf)` | **Exactly once** per received buffer; returns the slice(s) to the pool. |

If `on_data_zc` is registered, `on_data` is not called for the same message.

### Status structures

- **`shmipc_server_status_t`:** `is_running`, `connected_clients`.
- **`shmipc_session_status_t`:** `is_alive`, traffic counters for that client session, `send_buffer_used_pct` for server→client ring.
- **`shmipc_client_status_t`:** `is_connected`, traffic counters, `send_buffer_used_pct` for client→server ring.

### Latency statistics (`shmipc_latency_stats_t`)

Fields (all nanoseconds): `count`, `min_ns`, `avg_ns`, `p50_ns`, `p90_ns`, `p99_ns`, `p999_ns`, `max_ns`. Percentiles are approximate (log₂ buckets). `count == 0` means no samples yet.

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

### Multi-slice write zero-copy (same `alloc_buf` / `send_buf`)

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
./shmipc_test4_zc     2>/dev/null   # Zero-copy recv + write alloc_buf (C→S / S→C)
./shmipc_test5_latency              # Latency monitoring API
./shmipc_test7_dispatch             # Async dispatch (S→C and C→S)
```

Exit code `0` = all PASS, `1` = failure. See [`tests/README.md`](tests/README.md) for detailed descriptions.

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
- **Multi-slice `send_buf`:** when `alloc_buf` used more than one slice, **`send_buf(..., len)` must use the same `len` as in `alloc_buf`**.
- `channel_name` is a Unix Domain Socket abstract namespace path. Keep it ≤ 32 characters (letters, digits, underscores).
