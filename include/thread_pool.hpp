#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "task.hpp"
#include "work_stealing_queue.hpp"

// ThreadPool with per-worker work-stealing queues.
//
// Submit strategy:
//   1. If called from a worker thread, push directly into that worker's
//      local deque (avoids contention on the global queue).
//   2. Otherwise, push into the global queue.
//
// Worker loop:
//   1. Pop from own local deque.
//   2. Pop from global queue.
//   3. Steal from a random other worker's deque.
//   4. Sleep on condition_variable if nothing found.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit any callable; returns std::future<ReturnType>.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
    {
        using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        auto bound = [f = std::forward<F>(f),
                      ...a = std::forward<Args>(args)]() mutable -> R {
            return std::invoke(std::move(f), std::move(a)...);
        };

        auto pt  = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        auto fut = pt->get_future();
        Task task([pt = std::move(pt)]() mutable { (*pt)(); });

        enqueue(std::move(task));
        return fut;
    }

    // Submit a pre-built Task (used by TaskGraph).
    void submit_task(Task task) { enqueue(std::move(task)); }

    std::size_t thread_count() const { return workers_.size(); }

    // Wait until all submitted tasks have finished.
    void wait_all();

private:
    void worker_loop(std::size_t id);
    void enqueue(Task task);
    std::optional<Task> try_steal(std::size_t thief_id);

    std::vector<std::thread>                    workers_;
    std::vector<std::unique_ptr<WorkStealingQueue>> local_queues_;

    // Global queue for externally-submitted tasks
    std::mutex                  global_mutex_;
    std::deque<Task>            global_queue_;
    std::condition_variable     cv_;
    std::atomic<bool>           stop_{false};

    // For wait_all()
    std::atomic<int64_t>        pending_{0};
    std::mutex                  done_mutex_;
    std::condition_variable     done_cv_;

    // Thread-local index: which worker am I?
    static thread_local std::optional<std::size_t> worker_id_;
};
