# Changelog

All notable changes to this project will be documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).  
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

---

## [1.1.0] - 2026-04-30

### Fixed — Critical (crash / deadlock / security)

- **Handshake timeout deadlock** (`ShmClientSession::connect`): the timeout
  path called `stopThreads()` without first calling `shutdown(fd, SHUT_RDWR)`,
  so `readerThread` blocked forever in `recvmsg` and `join` never returned.
  Fix: call `shutdown()` before `stopThreads()` in the timeout branch, matching
  the behaviour of the normal `disconnect()` path.

- **Use-after-unmap / null-dereference race** (`ShmClientSession`,
  `ShmServerSession`): `cleanupSharedMemory()` zeroed the SHM buffer pointers
  *after* `munmap`, leaving a window where a concurrent write thread could
  dereference the already-unmapped region.  `sendWriteBuf`, `discardWriteBuf`,
  and `allocWriteBuf` also accessed SHM without holding the write mutex, so they
  could race with cleanup.  Fix: acquire `mWriteMutex` inside
  `cleanupSharedMemory()` before zeroing the pointers, and move all SHM
  accesses in `sendWriteBuf` / `discardWriteBuf` / `allocWriteBuf` under the
  same mutex with a validity check (`buf->manager == mClientWriteBuf`).

- **Server accepted SHM without size validation** (`ShmServerSession::
  handleAckShareMemoryMessage`): the server `mmap`-ed the client-supplied memfd
  at whatever size the kernel reported, without comparing against the size
  negotiated during the `ExchangeMetadata` handshake.  A malicious client could
  send a 1-byte fd and cause the server to read beyond it.  Fix: validate
  `st.st_size == mMetadata.shmSize` and reject the connection on mismatch.

- **Data race in free-list management** (`ShmBufferSlice::next`): `free_slice`
  performed a non-atomic write to `ShmBufferSlice::next` inside a CAS retry
  loop, while `alloc_slice` concurrently read the same field.  This is a formal
  C++ data race (UB).  Fix: change `ShmBufferSlice::next` from `uint32_t` to
  `std::atomic<uint32_t>` and update every access site to use
  `.load(relaxed)` / `.store(relaxed)` (the acquire-release ordering required
  for value visibility is already provided by the `free_head` CAS).

- **Ring-buffer head not advanced on consumer stop** (`readFromServerWriteBuffer`,
  `readFromClientWriteBuffer`): when the async dispatch queue was full and the
  consumer was shutting down, `goto done` exited the loop *after*
  `shmipc_buf_free(buf)` but *before* `queue->head.store(head + 1)`.  The slice
  was returned to the free list but the ring-buffer slot was left unconsumed,
  leaving the queue in an inconsistent state.  Fix: advance and store `head`
  immediately before `goto done`.

- **`shmipc_buf_decode` could OOM on malformed event length**
  (`ShmBufferManager.h`): `event.length` comes from shared memory and is
  peer-controlled.  A corrupt or malicious sender could set it to `0xFFFFFFFF`,
  causing `new uint8_t[~4 GB]` to throw `std::bad_alloc`.  Fix: validate
  `total_len` against `slice_count × slice_size` (the pool maximum); on
  failure, free the slice chain and return `nullptr` so the consumer skips the
  event cleanly.

### Fixed — High

- **`ShareMemoryManager` destructor leaked mmap and fd**: the destructor was
  `= default` and did nothing.  If the session was torn down before
  `cleanupSharedMemory()` was called, both the `mmap` mapping and the memfd
  were leaked.  Fix: implement a proper destructor that calls `munmap` and
  `close` when the sentinel values indicate the resources are still held; also
  changed `createShareMemory` parameter from `int` to `size_t` and added a
  size == 0 guard.

- **`apiHandle` leaked on server destroy** (`shmipc_api.cpp`): `shmipc_server_
  destroy` iterated over sessions *after* `stop()` had already cleared the
  session map, so the cleanup loop was always a no-op.  Fix: install an
  internal `onDisconnected` handler in `shmipc_server_create()` that always
  frees the `apiHandle`; `destroy()` no longer needs its own cleanup loop.

- **Payload-length integer underflow → OOM** (`ShmClientSession::readerThread`,
  `ShmServerSession::clientUdsReader`): `payloadLen = header.length -
  SHM_SERVER_PROTOCOL_HEAD_SIZE` is an unsigned subtraction.  When a malformed
  (or truncated) message arrives with `length < 7`, the result wraps to ~4 GB
  and `std::vector<char>(payloadLen)` throws OOM.  Fix: validate
  `header.length >= SHM_SERVER_PROTOCOL_HEAD_SIZE` before subtracting; drop
  the connection on failure.

- **Dead code in `shmipc_session_send_buf` / `shmipc_client_send_buf` leaked
  `wbuf`** (`shmipc_api.cpp`): the inner guard condition was logically
  contradictory with the outer guard, making the `discardWriteBuf` call
  unreachable.  When `s` was `nullptr` and `buf` was non-null, the buffer was
  silently leaked.  Fix: simplify the guards so that the discard path is
  reachable whenever the session type matches.

- **Deadlock: `cleanAllShmClient` / `removeDeadSessions` held `mClientMutex`
  while joining threads** (`ShmServerSessionManager`): the lock was held across
  `stopRunReadThreadLoop()` / `unique_ptr` destruction, both of which join
  background threads.  If an `onDisconnected` callback tried to call
  `getAllSessions()` or `getConnectedCount()`, it would deadlock on
  `mClientMutex`.  Fix: move sessions out of the map (under the lock), release
  the lock, then stop/destroy the sessions outside the critical section.

- **Data callback fired before `onConnected`** (`ShmServerSession::
  onSharedMemoryReady`, `ShmClientSession::handleShareMemoryReady`): the
  consumer thread was started before `onConnected` was invoked.  If data
  arrived immediately after the SHM handshake, `onData` could be delivered
  before the application had seen `onConnected`.  Separately, the server-side
  `get_or_create_server_session_handle()` was not thread-safe because both the
  `onConnected` and `onData` callbacks could call it concurrently.  Fix: invoke
  `onConnected` *before* starting the consumer thread on both sides; this
  ensures the `apiHandle` is fully initialised before any data callback runs.

- **`sendmsg` without `MSG_NOSIGNAL` could kill the process** (`ShmProtocol
  Handler::sendShmMessage`): writing to a closed peer socket delivered
  `SIGPIPE` to the process, whose default action is termination.  Fix: pass
  `MSG_NOSIGNAL` to all `sendmsg` calls, and handle `EPIPE` / `ECONNRESET`
  gracefully.  Also added a 100 µs back-off on `EAGAIN` to avoid a hot
  busy-loop.

- **SCM_RIGHTS file-descriptor could be lost across partial `recvmsg` reads**
  (`ShmProtocolHandler::receiveProtocolHeader`): a `control_parsed` flag
  prevented ancillary-data processing on iterations after the first, so any
  `SCM_RIGHTS` payload that arrived in a later `recvmsg` call was silently
  discarded (the kernel closed the passed fd).  Fix: remove the flag and parse
  ancillary data on every iteration; zero the control buffer between calls to
  prevent stale data from bleeding through.

### Fixed — Medium

- **Undefined behaviour in `ShmIpcMessageHeader::deserialize`**: `data[0] <<
  24` promotes `uint8_t` to `int` before shifting; when `data[0] >= 0x80` the
  shift overflows a signed 32-bit integer (UB).  Fix: cast each byte to
  `uint32_t` before shifting.

- **Ring-buffer usage percentage wrong on pointer wrap-around**
  (`ShmClientSession::getStatus`, `ShmServerSession::getStatus`): `tail -
  head` is an unsigned subtraction that wraps to ~4 GB when `tail < head`,
  causing the `used <= cap` guard to fail and the field to read back 0% when
  the queue was nearly full.  Fix: use `(tail - head + cap) % cap`.

### Changed — Build system

- **`bench_test.c` added to CMake build**: the file had a complete `main()`
  and test logic but was never compiled because it was missing from
  `CMakeLists.txt`.

- **CTest integration added**: `enable_testing()` and `add_test()` calls allow
  `ctest --output-on-failure` to drive the full test suite automatically.

- **`pthread` changed from `PUBLIC` to `PRIVATE`**: `pthread` is an
  implementation detail of the library and should not be propagated to
  consumers via `target_link_libraries`.

- **`SHMIPC_BUILD_TESTS` option moved to the top** of `CMakeLists.txt`,
  alongside the other options, and the duplicate declaration further down was
  removed.

- **`install.sh`: dynamic NDK host detection**: the `llvm-strip` path was
  hard-coded to `linux-x86_64`.  It now detects the host OS and architecture
  with `uname`, and maps `darwin-arm64` → `darwin-x86_64` (Apple Silicon uses
  the Rosetta NDK toolchain).

### Fixed — Tests

- **`test5_latency`: always exited 0** even when sub-tests failed.  Added a
  `g_failed` counter; `main()` now returns `g_failed ? 1 : 0`.

- **`test4_zc`: Section 1 & 2 failures not reflected in exit code**.  Only
  Section 3 (`wbuf_fail`) was checked.  Added `g_integ_failed` tracking and
  included it in the final `return` expression.

- **`test7_dispatch`: `stop_seen` written with a plain store in concurrent
  dispatch context** (4 threads).  Changed to `__atomic_store_n(...,
  __ATOMIC_RELEASE)` on write and `__atomic_load_n(..., __ATOMIC_ACQUIRE)` on
  read.

- **`test_common.h` `wait_flag`: used `volatile` without SMP memory barrier**.
  On weakly-ordered architectures (ARM64/Android), the signalling thread's
  prior writes are not guaranteed visible after a plain `volatile` read.
  Changed to `__atomic_load_n(flag, __ATOMIC_ACQUIRE)`.

- **`bench_test.c` `fmtsz`: single static buffer unsafe for multiple calls per
  `printf`**.  Replaced with a rotating 4-buffer matching the implementation
  already present in `test_common.h`.

### Changed — Documentation

- **`README.md` Notes section**: fixed Markdown syntax (`` `**xxx` `` mixed
  code/bold markers corrected to `**\`xxx\`**`).

- **Zero-copy receive examples**: added a **client-side** `on_data_zc` snippet
  to both READMEs.  The client callback type is `shmipc_cli_on_data_zc_cb`
  whose first argument is `shmipc_client_t*`, not `shmipc_session_t*`; this
  difference was previously undocumented by example.

- **"Query Status" section added to `README.md`**: was present in the Chinese
  README but absent from the English version.

- **Running Tests section updated**: added `shmipc_bench_test` to the binary
  list, added `ctest` usage, and noted that test6 was intentionally never
  assigned.

---

## [1.0.0] - 2024-01-01

### Added

- Initial release.
- Shared-memory IPC over Linux `memfd_create` + `FUTEX_WAIT/WAKE`.
- Zero-copy receive (`on_data_zc`) and write-side zero-copy (`alloc_buf` /
  `send_buf`) APIs.
- Async dispatch: serial (preserves order) and concurrent thread-pool modes.
- Latency histogram with nanosecond resolution (log₂ bucketing).
- Three configuration presets: `LOW_FREQ`, `GENERAL`, `HIGH_THROUGHPUT`.
- Android arm64-v8a cross-compilation support via NDK CMake toolchain.
- `install.sh` packaging script producing a `dist/` tree for each ABI.
- Integration test suite: `test1_s2c`, `test2_c2s`, `test3_duplex`,
  `test4_zc`, `test5_latency`, `test7_dispatch`.

---
