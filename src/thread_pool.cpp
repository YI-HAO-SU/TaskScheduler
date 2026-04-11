#include "thread_pool.hpp"

#include <cassert>

thread_local std::optional<std::size_t> ThreadPool::worker_id_ = std::nullopt;

ThreadPool::ThreadPool(std::size_t num_threads) {
    assert(num_threads > 0);

    for (std::size_t i = 0; i < num_threads; ++i)
        local_queues_.push_back(std::make_unique<WorkStealingQueue>());

    for (std::size_t i = 0; i < num_threads; ++i)
        workers_.emplace_back(&ThreadPool::worker_loop, this, i);
}

ThreadPool::~ThreadPool() {
    stop_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::enqueue(Task task) {
    pending_.fetch_add(1, std::memory_order_relaxed);

    // If called from a worker, push to that worker's local deque.
    if (worker_id_.has_value()) {
        local_queues_[*worker_id_]->push(std::move(task));
        cv_.notify_one();
        return;
    }

    // External submission: push to global queue.
    {
        std::lock_guard lock(global_mutex_);
        global_queue_.push_back(std::move(task));
    }
    cv_.notify_one();
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

        // 1. Own local deque
        task = local_queues_[id]->pop();

        // 2. Global queue
        if (!task) {
            std::lock_guard lock(global_mutex_);
            if (!global_queue_.empty()) {
                task = std::move(global_queue_.front());
                global_queue_.pop_front();
            }
        }

        // 3. Steal from another worker
        if (!task) task = try_steal(id);

        if (task) {
            (*task)();
            int64_t prev = pending_.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) done_cv_.notify_all();
            continue;
        }

        // Nothing found — sleep until woken
        std::unique_lock lock(global_mutex_);
        cv_.wait(lock, [&] {
            return stop_.load(std::memory_order_relaxed) ||
                   !global_queue_.empty() ||
                   !local_queues_[id]->empty();
        });

        if (stop_.load(std::memory_order_relaxed) &&
            global_queue_.empty() &&
            local_queues_[id]->empty())
            break;
    }
}

void ThreadPool::wait_all() {
    std::unique_lock lock(done_mutex_);
    done_cv_.wait(lock, [&] {
        return pending_.load(std::memory_order_acquire) == 0;
    });
}
