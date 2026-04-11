#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "task.hpp"

// Lock-free work-stealing deque (Chase-Lev algorithm).
//
// The *owner* thread pushes/pops from the bottom (LIFO, cache-friendly).
// *Thief* threads steal from the top (FIFO).
//
// Memory ordering rationale:
//   - bottom_: only written by owner  → relaxed store/load by owner
//   - top_:    written by thieves     → CAS with acq_rel / acquire
//   - Circular buffer resize uses     → seq_cst fence on publish
//
// Reference: "Dynamic Circular Work-Stealing Deque" (Chase & Lev, 2005)
class WorkStealingQueue {
public:
    explicit WorkStealingQueue(std::size_t initial_capacity = 256)
        : top_(0), bottom_(0)
    {
        assert((initial_capacity & (initial_capacity - 1)) == 0 &&
               "capacity must be a power of 2");
        buffer_.store(new Buffer(initial_capacity), std::memory_order_relaxed);
    }

    ~WorkStealingQueue() {
        delete buffer_.load(std::memory_order_relaxed);
    }

    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    // Called by owner thread: push a task onto the bottom.
    void push(Task task) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);
        Buffer*  buf = buffer_.load(std::memory_order_relaxed);

        if (b - t >= static_cast<int64_t>(buf->capacity() - 1)) {
            // Grow: allocate 2x buffer and migrate tasks.
            Buffer* new_buf = buf->grow(b, t);
            delete buf;
            buf = new_buf;
            buffer_.store(buf, std::memory_order_relaxed);
        }

        buf->store(b, std::move(task));
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // Called by owner thread: pop from the bottom (LIFO).
    std::optional<Task> pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        Buffer* buf = buffer_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            // Non-empty
            Task task = buf->load(b);
            if (t == b) {
                // Last item — race with thieves
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    // Lost the race
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return task;
        }

        // Empty
        bottom_.store(b + 1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // Called by thief threads: steal from the top (FIFO).
    std::optional<Task> steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);

        if (t >= b) return std::nullopt;   // Empty

        Buffer* buf = buffer_.load(std::memory_order_consume);
        Task    task = buf->load(t);

        if (!top_.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return std::nullopt;   // Lost the race
        }
        return task;
    }

    bool empty() const {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return b <= t;
    }

    std::size_t size() const {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(b > t ? b - t : 0);
    }

private:
    // Circular buffer that stores Tasks by index (mod capacity).
    struct Buffer {
        explicit Buffer(std::size_t cap)
            : mask_(cap - 1), slots_(cap) {}

        std::size_t capacity() const { return mask_ + 1; }

        void store(int64_t idx, Task task) {
            slots_[idx & mask_] = std::move(task);
        }

        Task load(int64_t idx) {
            return std::move(slots_[idx & mask_]);
        }

        // Allocate 2x buffer and move live tasks.
        Buffer* grow(int64_t bottom, int64_t top) {
            auto* nb = new Buffer(capacity() * 2);
            for (int64_t i = top; i < bottom; ++i)
                nb->slots_[i & nb->mask_] = std::move(slots_[i & mask_]);
            return nb;
        }

        std::size_t             mask_;
        std::vector<Task>       slots_;
    };

    std::atomic<int64_t>   top_;
    std::atomic<int64_t>   bottom_;
    std::atomic<Buffer*>   buffer_;
};
