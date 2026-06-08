#include "thread_pool.hpp"

#include <cassert>

thread_local std::optional<std::size_t> ThreadPool::worker_id_ = std::nullopt;

ThreadPool::ThreadPool(std::size_t num_threads) {
    assert(num_threads > 0);

    for (std::size_t i = 0; i < num_threads; ++i) {
        local_queues_.push_back(std::make_unique<WorkStealingQueue>());
        inboxes_.push_back(std::make_unique<Inbox>());
    }

    for (std::size_t i = 0; i < num_threads; ++i)
        workers_.emplace_back(&ThreadPool::worker_loop, this, i);
}

ThreadPool::~ThreadPool() {
    stop_.store(true, std::memory_order_relaxed);   // Wake up all workers so they can exit.
    cv_.notify_all();   // Join all worker threads.
    for (auto& t : workers_) t.join();  // Wait for all workers to finish.
}

void ThreadPool::enqueue(Task task) {
    pending_.fetch_add(1, std::memory_order_relaxed);

    if (worker_id_.has_value()) {
        // Worker submits directly to its own Chase-Lev deque (owner-only push).
        local_queues_[*worker_id_]->push(std::move(task));
        queued_.fetch_add(1, std::memory_order_release);
        cv_.notify_one();
    } else {
        // External submit: round-robin into per-worker inbox.
        // notify_all() ensures the target worker (not an arbitrary one) wakes up,
        // since only that worker drains its own inbox.
        std::size_t idx = round_robin_idx_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
        {
            std::lock_guard lk(inboxes_[idx]->mtx);
            inboxes_[idx]->tasks.push_back(std::move(task));
        }
        queued_.fetch_add(1, std::memory_order_release);
        cv_.notify_all();
    }
}

std::optional<Task> ThreadPool::try_steal(std::size_t thief_id) {
    const std::size_t n = local_queues_.size();
    thread_local std::mt19937 rng{std::random_device{}()};
    std::size_t start = rng() % n;

    for (std::size_t i = 0; i < n; ++i) {
        std::size_t victim = (start + i) % n;
        if (victim == thief_id) continue;
        if (auto t = local_queues_[victim]->steal())
            return t;
    }
    return std::nullopt;
}

void ThreadPool::worker_loop(std::size_t id) {
    worker_id_ = id;

    while (true) {
        std::optional<Task> task;

        // 1. Own Chase-Lev deque (worker-produced tasks, LIFO)
        task = local_queues_[id]->pop();

        // 2. Own inbox (externally submitted tasks, round-robin)
        if (!task) {
            std::lock_guard lk(inboxes_[id]->mtx);
            if (!inboxes_[id]->tasks.empty()) {
                task = std::move(inboxes_[id]->tasks.front());
                inboxes_[id]->tasks.pop_front();
            }
        }

        // 3. Steal from another worker's Chase-Lev deque
        if (!task) task = try_steal(id);

        if (task) {
            // Decrement before execution: task has left all queues.
            queued_.fetch_sub(1, std::memory_order_relaxed);
            (*task)();
            int64_t prev = pending_.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) done_cv_.notify_all();
            continue;
        }

        // Nothing found — sleep on sleep_mutex_ (not global_mutex_) so we don't
        // serialize N workers on a single lock during wakeup (issue #3).
        // Predicate is a single atomic load — no queue scanning.
        std::unique_lock lock(sleep_mutex_);
        cv_.wait(lock, [&] {
            return stop_.load(std::memory_order_relaxed) ||
                   queued_.load(std::memory_order_acquire) > 0;
        });

        if (stop_.load(std::memory_order_relaxed) &&
            queued_.load(std::memory_order_relaxed) == 0)
            break;
    }
}

void ThreadPool::wait_all() {
    std::unique_lock lock(done_mutex_);
    done_cv_.wait(lock, [&] {
        return pending_.load(std::memory_order_acquire) == 0;
    });
}
