#ifndef SHMIPC_SHMDISPATCHQUEUE_H
#define SHMIPC_SHMDISPATCHQUEUE_H

/* -----------------------------------------------------------------------
 *  ShmDispatchQueue — bounded, blocking single-consumer dispatch queue
 *
 *  The SHM consumer thread pushes decoded shmipc_buf_t* items here
 *  (non-blocking; returns false if full).  A dedicated dispatch thread
 *  pops items (blocking) and calls the application callbacks.
 *
 *  This decouples the fast SHM ring-buffer drain from slow app callbacks:
 *  a slow on_data handler no longer stalls the producer.
 * --------------------------------------------------------------------- */

#include <deque>
#include <mutex>
#include <condition_variable>
#include <cstdint>

#include "shmipc/ShmBufferManager.h"  /* shmipc_buf_t, shmipc_buf_free */

class ShmDispatchQueue {
public:
    explicit ShmDispatchQueue(uint32_t max_depth)
        : max_(max_depth) {}

    /* Non-blocking push.
     * Returns false if the queue is full or has been stopped.
     * The caller retains ownership of buf on false. */
    bool try_push(shmipc_buf_t* item) {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_ || q_.size() >= max_) return false;
        q_.push_back(item);
        cv_.notify_one();
        return true;
    }

    /* Blocking pop.
     * Returns nullptr when the queue has been stopped and is empty. */
    shmipc_buf_t* pop() {
        std::unique_lock<std::mutex> g(mu_);
        cv_.wait(g, [this] { return stopped_ || !q_.empty(); });
        if (q_.empty()) return nullptr;
        auto* item = q_.front();
        q_.pop_front();
        return item;
    }

    /* Stop the queue and wake all blocked pop() callers. */
    void stop() {
        std::lock_guard<std::mutex> g(mu_);
        stopped_ = true;
        cv_.notify_all();
    }

    bool is_full() const {
        std::lock_guard<std::mutex> g(mu_);
        return q_.size() >= max_;
    }

    uint32_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return static_cast<uint32_t>(q_.size());
    }

private:
    const uint32_t         max_;
    std::deque<shmipc_buf_t*> q_;
    mutable std::mutex     mu_;
    std::condition_variable cv_;
    bool                   stopped_ = false;
};

#endif /* SHMIPC_SHMDISPATCHQUEUE_H */
