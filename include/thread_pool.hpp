#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
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
//      local deque.
//   2. Otherwise, round-robin across all local deques (no global queue,
//      no mutex on the submit path).
//
// Worker loop:
//   1. Pop from own local deque.
//   2. Steal from a random other worker's deque.
//   3. Sleep on condition_variable if nothing found.
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

        // Single allocation: FutureTask embeds packaged_task directly so we
        // avoid the extra shared_ptr + ConcreteTask wrapping (issue #5).
        struct FutureTask : Task::TaskBase {
            std::packaged_task<R()> pt;
            explicit FutureTask(decltype(bound) b) : pt(std::move(b)) {}
            void invoke() override { pt(); }
        };
        auto impl = std::make_unique<FutureTask>(std::move(bound));
        auto fut  = impl->pt.get_future();

        // Upcast to TaskBase so Task(unique_ptr<TaskBase>) is chosen over Task(F&&).
        enqueue(Task(std::unique_ptr<Task::TaskBase>(std::move(impl))));
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

    std::vector<std::thread>                         workers_;
    std::vector<std::unique_ptr<WorkStealingQueue>>  local_queues_;

    // Per-worker inbox for external submits.
    // Chase-Lev push() is owner-only; external threads must go through a
    // separate inbox so they never race with the owner's push/pop on bottom_.
    struct Inbox {
        alignas(64) std::mutex   mtx;
        std::deque<Task>         tasks;
    };
    std::vector<std::unique_ptr<Inbox>>  inboxes_;

    std::mutex               sleep_mutex_;
    std::condition_variable  cv_;
    std::atomic<bool>        stop_{false};

    // Round-robin index for external submits: wraps around worker count.
    alignas(64) std::atomic<std::size_t> round_robin_idx_{0};
    // Total items across all queues: O(1) sleep predicate.
    alignas(64) std::atomic<int64_t>     queued_{0};

    // For wait_all()
    alignas(64) std::atomic<int64_t>  pending_{0};
    std::mutex               done_mutex_;
    std::condition_variable  done_cv_;

    // Thread-local index: which worker am I?
    static thread_local std::optional<std::size_t> worker_id_;
};
