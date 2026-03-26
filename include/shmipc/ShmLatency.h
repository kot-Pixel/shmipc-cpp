#ifndef SHMIPC_SHMLATENCY_H
#define SHMIPC_SHMLATENCY_H

#include <atomic>
#include <cstdint>
#include <climits>

#include "shmipc/shmipc.h"   /* shmipc_latency_stats_t */

/* -----------------------------------------------------------------------
 *  LatencyHistogram
 *
 *  Lock-free, multi-writer-safe power-of-2 bucket histogram.
 *
 *  64 buckets: bucket[k] counts latency samples in [2^k ns, 2^(k+1) ns).
 *  Representative bucket ranges for common IPC latencies:
 *    k= 9 → [512 ns,  1 µs)
 *    k=10 → [  1 µs,  2 µs)
 *    k=13 → [  8 µs, 16 µs)
 *    k=16 → [ 65 µs, 131 µs)
 *    k=19 → [524 µs,  1 ms)
 *    k=20 → [  1 ms,  2 ms)
 *    k=23 → [  8 ms, 16 ms)
 *
 *  Percentiles are approximate (resolution ±1 bucket = ±2× within range).
 *  For diagnostic use this is entirely sufficient.
 * --------------------------------------------------------------------- */
class LatencyHistogram {
public:
    static constexpr int BUCKETS = 64;

    /* Record one latency sample (in nanoseconds). Thread-safe. */
    void record(uint64_t ns) {
        if (ns == 0) ns = 1;

        /* floor(log2(ns)) via count-leading-zeros */
        int k = 63 - __builtin_clzll(ns);
        if (k < 0)        k = 0;
        if (k >= BUCKETS) k = BUCKETS - 1;

        buckets_[k].fetch_add(1, std::memory_order_relaxed);
        count_  .fetch_add(1,  std::memory_order_relaxed);
        sum_    .fetch_add(ns, std::memory_order_relaxed);

        /* min — CAS loop (fine under low contention) */
        uint64_t cur = min_ns_.load(std::memory_order_relaxed);
        while (ns < cur &&
               !min_ns_.compare_exchange_weak(cur, ns, std::memory_order_relaxed))
            ;

        /* max */
        cur = max_ns_.load(std::memory_order_relaxed);
        while (ns > cur &&
               !max_ns_.compare_exchange_weak(cur, ns, std::memory_order_relaxed))
            ;
    }

    /* Reset all counters atomically (best-effort — not a snapshot boundary). */
    void reset() {
        for (int i = 0; i < BUCKETS; ++i)
            buckets_[i].store(0, std::memory_order_relaxed);
        count_ .store(0,         std::memory_order_relaxed);
        sum_   .store(0,         std::memory_order_relaxed);
        min_ns_.store(UINT64_MAX, std::memory_order_relaxed);
        max_ns_.store(0,          std::memory_order_relaxed);
    }

    /* Snapshot current statistics into *out. */
    void get(shmipc_latency_stats_t* out) const {
        if (!out) return;

        uint64_t total = count_.load(std::memory_order_acquire);
        out->count  = total;
        out->min_ns = (total > 0) ? min_ns_.load(std::memory_order_relaxed) : 0;
        out->max_ns = max_ns_.load(std::memory_order_relaxed);
        out->avg_ns = (total > 0)
                      ? (sum_.load(std::memory_order_relaxed) / total) : 0;

        if (total == 0) {
            out->p50_ns = out->p90_ns = out->p99_ns = out->p999_ns = 0;
            return;
        }

        /* Snapshot bucket counts once for consistent percentile walk. */
        uint64_t snap[BUCKETS];
        for (int i = 0; i < BUCKETS; ++i)
            snap[i] = buckets_[i].load(std::memory_order_relaxed);

        uint64_t actual_max = out->max_ns;

        auto percentile_ns = [&](double pct) -> uint64_t {
            /* ceiling(total * pct/100), minimum 1 */
            uint64_t target = static_cast<uint64_t>(
                static_cast<double>(total) * pct / 100.0 + 0.5);
            if (target == 0) target = 1;

            uint64_t acc = 0;
            for (int i = 0; i < BUCKETS; ++i) {
                acc += snap[i];
                if (acc >= target) {
                    /* Representative: midpoint of [2^i, 2^(i+1)).
                     * Cap at actual max so percentile ≤ max always holds. */
                    uint64_t lo  = (1ULL << i);
                    uint64_t hi  = (i < 63) ? (1ULL << (i + 1)) : UINT64_MAX;
                    uint64_t rep = lo + (hi - lo) / 2;
                    return (rep < actual_max) ? rep : actual_max;
                }
            }
            return actual_max;
        };

        out->p50_ns  = percentile_ns(50.0);
        out->p90_ns  = percentile_ns(90.0);
        out->p99_ns  = percentile_ns(99.0);
        out->p999_ns = percentile_ns(99.9);
    }

private:
    std::atomic<uint64_t> buckets_[BUCKETS]{};
    std::atomic<uint64_t> count_ {0};
    std::atomic<uint64_t> sum_   {0};
    std::atomic<uint64_t> min_ns_{UINT64_MAX};
    std::atomic<uint64_t> max_ns_{0};
};

#endif /* SHMIPC_SHMLATENCY_H */
