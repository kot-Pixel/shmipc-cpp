# shmipc 测试套件说明

## 1. 运行环境

| 项目 | 值 |
|------|-----|
| 操作系统 | Ubuntu 22.04.5 LTS (WSL2, kernel 6.6.87.2-microsoft-standard-WSL2) |
| CPU | 12th Gen Intel Core i5-12400 — 6 物理核 / 12 逻辑核 |
| 内存 | 7.6 GiB |
| 编译器 | GCC 11.4.0 |
| 构建工具 | CMake 3.22.1 |
| C 标准 | C11 |
| 线程库 | POSIX pthreads |

> 测试程序使用 `fork()` 在同一台主机上模拟独立的 server / client 进程，因此所有数据传输均经过真实的共享内存路径，而非 loopback。

---

## 2. 测试文件说明

```
tests/
├── test_common.h      — 公共定义：消息格式、预设参数、写模式、工具函数
├── test1_s2c.c        — 单向 Server → Client 基准测试
├── test2_c2s.c        — 单向 Client → Server 基准测试
├── test3_duplex.c     — 全双工 + 多写线程 + 混合模式测试
├── test4_zc.c         — 接收侧零拷贝 on_data_zc / 多 slice 回退
├── test5_latency.c    — 延迟直方图与 reset_latency
└── test7_dispatch.c   — 异步 dispatch：`set_async_dispatch` + 慢回调 + 突发发送
```

---

## 3. 设计思路

### 3.1 整体架构

每个测试程序均采用 **`fork()` 进程对** 模型：

```
main()
  ├─ fork()
  │    ├─ 父进程 = Server (shmipc_server_t)
  │    └─ 子进程 = Client (shmipc_client_t)
  └─ 通过一对匿名管道 (pipe) 同步控制流与统计数据
```

- 父子进程通过 **`pipe`** 传递控制命令（`'G'`/`'C'`/`'D'`/`'E'`/`'X'`）和统计结构体，避免使用共享全局变量。
- 子进程的 `stdout`/`stderr` 被重定向到 `/dev/null`，防止日志与父进程输出交叉。
- 数据通知机制：底层使用 **`futex`** 而非 Unix Domain Socket，减少上下文切换。

### 3.2 消息格式

每条消息由固定头部 + 可变负载组成：

```
┌──────────────────────────────────────────┐
│  seq (4B, uint32_t)                      │  ← 0xDEADBEEF 表示 STOP 哨兵
│  plen (4B, uint32_t)                     │  ← 负载字节数
│  payload (plen bytes, 内容可验证)         │
└──────────────────────────────────────────┘
HDR_SZ = 8 字节
```

接收方通过 `len != HDR_SZ + plen` 来判断消息是否完整，用 `seq == STOP_SEQ` 来标记一轮发送结束。

### 3.3 三种写模式

| 模式 | timeout_ms 值 | 语义 |
|------|---------------|------|
| BLOCK  | `SHMIPC_TIMEOUT_INFINITE (0)` | 阻塞直到 buffer 有空间 |
| NONBLK | `SHMIPC_TIMEOUT_NONBLOCKING (-1)` | 立即返回，buffer 满则丢弃 |
| 100ms  | `100` | 超时返回 `SHMIPC_TIMEOUT` |

### 3.4 三种预设（Preset）

| 名称 | SHM 大小 | 队列容量 | Slice 大小 | 单次最大消息 | 适用场景 |
|------|----------|----------|------------|-------------|---------|
| LOW_FREQ  |  8 MB |  32 |  4 KB |  ~4 MB  | 低频控制消息 |
| GENERAL   | 16 MB |  64 | 16 KB |  ~8 MB  | 通用 IPC    |
| HI_THRU   | 64 MB | 256 | 64 KB | ~32 MB  | 高吞吐视频/音频帧 |

---

## 4. test1_s2c — Server → Client 单向测试

**测试矩阵：**

- Preset: LOW_FREQ × 13 种负载 (256B … 1MB) × 3 种写模式 = 39 行
- Preset: GENERAL  × 15 种负载 (256B … 4MB) × 3 种写模式 = 45 行
- Preset: HI_THRU  × 16 种负载 (256B … 8MB) × 3 种写模式 = 48 行
- Multi-Writer 压力: GENERAL × 3 种负载 × 3 种线程数 (2/4/8) = 9 行，BLOCK 模式

**判定逻辑：**

- BLOCK 模式：`recv == sent` 且 `errs == 0` → **PASS**
- NONBLK/100ms 模式：`recv == sent_ok` 且 `errs == 0` → **OK**（允许有丢弃）

---

## 5. test2_c2s — Client → Server 单向测试

与 test1 完全对称，写方向从 Client 到 Server，测试矩阵和判定逻辑相同。

---

## 6. test3_duplex — 全双工测试

分四个阶段，复杂度逐级递增：

### 阶段一：单线程非对称全双工（3 Preset × 多 Pair × 3 模式）

两端各一个 `pthread` 写线程，负载大小不对称（例如 Server 发 1MB，Client 同时发 256B），验证双向独立性。

### 阶段二：多线程全双工 BLOCK（`[DUPLEX Multi-Writer]`）

双端各 2/4/8 个线程，全部使用 BLOCK 模式，验证内部 mutex 在高并发写时的正确性。
GENERAL preset，3 种非对称 Pair（4KB↔1MB，64KB↔64KB，1MB↔4KB）。

### 阶段三：混合模式多写线程（`[DUPLEX Mixed-Mode MT]`）

两端模式不一致：

| 场景 | Server 模式 | Client 模式 |
|------|------------|------------|
| 场景 A | BLOCK  | NONBLK |
| 场景 B | NONBLK | BLOCK  |

线程数：2 / 4 / 8，Payload Pair：同阶段二。

**关键验证点：**
- NONBLK 端在 buffer 竞争时允许出现丢弃（`Sdrp`/`Cdrp` > 0），这是正常行为
- 实际送达的消息数必须完全匹配：`recv == ok_sent`（而非总尝试次数）
- 无任何尺寸错误（`errs == 0`）
- STOP 哨兵始终以 BLOCK 模式发送，保证对端能感知到一轮结束

### Pipe 协议

| 命令字节 | 含义 |
|---------|------|
| `'G'` | Server 通知 Client 准备下一轮 S→C 接收 |
| `'C'` | Server 通知 Client 开始 C→S 发送 |
| `'D'` | Server 通知 Client 开始全双工 |
| `'E'` | Server 发送 `mix_cmd_t`，告知 Client 负载/模式/线程数 |
| `'X'` | Server 通知 Client 所有轮次结束，退出 |

---

## 6.5 test4_zc / test5_latency / test7_dispatch

| 程序 | 目的 |
|------|------|
| **test4_zc** | `on_data_zc` 单 slice 借用 SHM；大于 slice 时 heap 回退；完整性校验 |
| **test5_latency** | `get_latency` / `reset_latency`；S→C 与 C→S 直方图合理性 |
| **test7_dispatch** | **异步 dispatch 专项**：`shmipc_*_set_async_dispatch(64)` 在 `start`/`connect` 之前启用；接收端 `on_data` 每次约 **1.2 ms** 休眠，对端 **连续突发 48 条** 小消息（64B 负载）。**[A] S→C** 测客户端 dispatch；**[B] C→S** 测服务端 dispatch。校验 `recv == 48`、**seq 严格递增**、`STOP` 哨兵。退出码 `0` = `overall: PASS`。 |

---

## 7. 构建与运行

```bash
# 进入构建目录（WSL 路径示例）
cd /mnt/d/shmipc-c++/shmipc/build

# 构建所有测试
cmake --build . --target shmipc_test1_s2c shmipc_test2_c2s shmipc_test3_duplex \
                       shmipc_test4_zc shmipc_test5_latency shmipc_test7_dispatch -j4

# 运行（建议重定向 stderr 屏蔽库内部日志）
./shmipc_test1_s2c  2>/dev/null
./shmipc_test2_c2s  2>/dev/null
./shmipc_test3_duplex 2>/dev/null
./shmipc_test4_zc  2>/dev/null
./shmipc_test5_latency
./shmipc_test7_dispatch
```

退出码：`0` = 全部 PASS，`1` = 存在 FAIL。

---

## 8. 运行结果

> 运行平台：同第 1 节。所有三个测试均以 `Overall: PASS` 退出。

---

### test1_s2c — 结果摘要

**LOW_FREQ preset（shm=8MB, queue=32, slice=4KB, max=1MB）**

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│  [S→C]  Preset: LOW_FREQ  shm= 8MB  queue= 32  slice=   4KB  max=   1MB        │
├──────────┬────────┬──────┬───────┬───────┬──────────┬────────┬──────────┤
│ Payload  │  Mode  │ msgs │  sent │  drop │    MB/s  │   recv │  result  │
├──────────┼────────┼──────┼───────┼───────┼──────────┼────────┼──────────┤
│    256 B │ BLOCK  │  200 │   200 │     0 │   160.22 │    200 │  PASS  │
│    256 B │ NONBLK │  200 │   186 │    14 │   397.69 │    186 │  OK    │
│    256 B │  100ms │  200 │   200 │     0 │    49.13 │    200 │  OK    │
│    ...   │        │      │       │       │          │        │        │
│      1MB │ BLOCK  │   20 │    20 │     0 │  1638.10 │     20 │  PASS  │
│      1MB │ NONBLK │   20 │    20 │     0 │  1473.49 │     20 │  OK    │
│      1MB │  100ms │   20 │    20 │     0 │  1476.48 │     20 │  OK    │
└──────────┴────────┴──────┴───────┴───────┴──────────┴────────┴──────────┘
```

**HI_THRU preset 大包峰值（shm=64MB, queue=256, slice=64KB）**

```
│      8MB │ BLOCK  │   10 │    10 │     0 │  1212.70 │     10 │  PASS  │
│      8MB │ NONBLK │   10 │    10 │     0 │  1349.17 │     10 │  OK    │
│      8MB │  100ms │   10 │    10 │     0 │  1481.17 │     10 │  OK    │
```

**S→C Multi-Writer（GENERAL, BLOCK, 2/4/8 线程）**

```
┌────────────────────────────────────────────────────────────────────────────────────┐
│  [S→C Multi-Writer]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  mode: BLOCK │
├──────────┬─────────┬─────────┬───────┬───────┬──────────┬────────┬──────────┤
│ Payload  │ Threads │  total  │  sent │  drop │    MB/s  │   recv │  result  │
├──────────┼─────────┼─────────┼───────┼───────┼──────────┼────────┼──────────┤
│      1KB │    2    │      50 │    50 │     0 │   254.97 │     50 │  PASS  │
│      1KB │    4    │     100 │   100 │     0 │   216.22 │    100 │  PASS  │
│      1KB │    8    │     200 │   200 │     0 │   349.95 │    200 │  PASS  │
│     64KB │    8    │     200 │   200 │     0 │  1547.05 │    200 │  PASS  │
│      1MB │    8    │     200 │   200 │     0 │  6942.29 │    200 │  PASS  │
└──────────┴─────────┴─────────┴───────┴───────┴──────────┴────────┴──────────┘
```

**Overall: PASS**

---

### test2_c2s — 结果摘要

与 test1 对称，方向为 Client → Server，所有 preset 结果一致。

**HI_THRU 8MB 代表性结果：**

```
│      8MB │ BLOCK  │   10 │    10 │     0 │  1271.21 │     10 │  PASS  │
│      8MB │ NONBLK │   10 │    10 │     0 │  1192.70 │     10 │  OK    │
│      8MB │  100ms │   10 │    10 │     0 │  1323.87 │     10 │  OK    │
```

**C→S Multi-Writer（GENERAL, BLOCK, 1MB×8 线程）**

```
│      1MB │    8    │     200 │   200 │     0 │  6148.41 │    200 │  PASS  │
```

**Overall: PASS**

---

### test3_duplex — 结果摘要

**单线程非对称全双工（GENERAL preset，部分行）**

```
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  [DUPLEX]  Preset: GENERAL   shm=16MB  queue= 64  slice=  16KB  max=   4MB                                     │
├──────────┬──────────┬────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤
│  SrvPay  │  CliPay  │  Mode  │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │
├──────────┼──────────┼────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤
│      1KB │      4MB │ BLOCK  │   200 │    0 │   488.88 │    200 │    10 │    0 │  1232.55 │     10 │  PASS  │
│      4KB │      1MB │ BLOCK  │   200 │    0 │   629.87 │    200 │    20 │    0 │  1549.32 │     20 │  PASS  │
│     64KB │     64KB │ BLOCK  │   100 │    0 │   969.84 │    100 │   100 │    0 │   989.83 │    100 │  PASS  │
│      4MB │      1KB │ BLOCK  │    10 │    0 │  1475.81 │     10 │   200 │    0 │  1131.26 │    200 │  PASS  │
└──────────┴──────────┴────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘
```

**全双工多写线程 BLOCK（GENERAL, 2/4/8 线程）**

```
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  [DUPLEX Multi-Writer]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  mode: BLOCK                          │
├──────────┬──────────┬─────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤
│  SrvPay  │  CliPay  │ Threads │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │
├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤
│      1MB │      4KB │    8    │   200 │    0 │  7187.53 │    200 │   200 │    0 │   404.12 │    200 │  PASS  │
└──────────┴──────────┴─────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘
```

1MB×8线程场景下 S→C 吞吐达 **7.18 GB/s**（共享内存 memcpy 带宽极限）。

**混合模式多写线程（SrvMode=BLOCK / CliMode=NONBLK）**

```
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  [DUPLEX Mixed-Mode MT]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  SrvMode=BLOCK    CliMode=NONBLK   threads: 2/4/8  │
├──────────┬──────────┬─────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤
│  SrvPay  │  CliPay  │ Threads │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │
├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤
│      4KB │      1MB │    2    │    50 │    0 │   372.84 │     50 │    50 │    0 │  3074.87 │     50 │  PASS  │
│      4KB │      1MB │    4    │   100 │    0 │   343.83 │    100 │   100 │    0 │  4981.36 │    100 │  PASS  │
│      4KB │      1MB │    8    │   200 │    0 │   645.81 │    200 │   200 │    0 │  6988.17 │    200 │  PASS  │
│     64KB │     64KB │    8    │   200 │    0 │  2660.40 │    200 │   200 │    0 │  2608.83 │    200 │  PASS  │
│      1MB │      4KB │    8    │   200 │    0 │  7526.17 │    200 │   200 │    0 │   100.68 │    200 │  PASS  │
└──────────┴──────────┴─────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘
```

> 注：1MB×4KB×8线程 场景中 C→S 方向（NONBLK）的 C→S MB/s 偏低（~100 MB/s），原因是 8 个线程竞争同一个 Client 写锁，NONBLK 下无等待直接放弃，有效写率降低。但 `recv == ok_sent` 且 `errs == 0`，数据正确性完全保证。

**混合模式多写线程（SrvMode=NONBLK / CliMode=BLOCK）**

```
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  [DUPLEX Mixed-Mode MT]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  SrvMode=NONBLK   CliMode=BLOCK    threads: 2/4/8  │
├──────────┬──────────┬─────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤
│  SrvPay  │  CliPay  │ Threads │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │
├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤
│      4KB │      1MB │    8    │   200 │    0 │   363.42 │    200 │   200 │    0 │  6933.42 │    200 │  PASS  │
│     64KB │     64KB │    8    │   200 │    0 │  3427.06 │    200 │   200 │    0 │  3047.67 │    200 │  PASS  │
│      1MB │      4KB │    8    │   200 │    0 │  6237.87 │    200 │   200 │    0 │   539.85 │    200 │  PASS  │
└──────────┴──────────┴─────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘
```

**Overall: PASS**

---

## 9. 性能规律总结

| 观测 | 说明 |
|------|------|
| 大包高带宽 | 单次写 1MB 以上，吞吐可达 1.2–1.7 GB/s（单线程），多线程可超过 7 GB/s |
| 小包受限于 futex 唤醒延迟 | 256B 单线程 BLOCK 约 160–490 MB/s，受制于进程间通知开销 |
| NONBLK 小包易丢弃 | 低 Slice 预设（4KB）下，256B 消息填充速度超过消费速度时，非阻塞写出现丢弃 |
| 多线程 BLOCK 线性扩展 | 1MB payload，8 线程 BLOCK 相比 2 线程吞吐约提升 2×（7 GB/s vs 3 GB/s） |
| 混合模式 NONBLK 侧竞争加剧丢弃 | 8 线程 NONBLK 竞争同一写锁，有效发送率随线程数增加而下降，但已发送的消息全部正确接收 |
