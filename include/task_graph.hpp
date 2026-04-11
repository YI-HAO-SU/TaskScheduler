#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "thread_pool.hpp"

// DAG-based task scheduler.
//
// Usage:
//   TaskGraph graph(pool);
//   auto a = graph.add_task("A", []{ /* ... */ });
//   auto b = graph.add_task("B", []{ /* ... */ });
//   auto c = graph.add_task("C", []{ /* ... */ });
//   graph.add_dependency(c, a);   // C depends on A
//   graph.add_dependency(c, b);   // C depends on B
//   graph.run();                  // A and B run in parallel; C runs after both
//   graph.wait();
//
// Cycle detection is performed at run() time via DFS.
class TaskGraph {
public:
    using TaskId = std::size_t;

    explicit TaskGraph(ThreadPool& pool) : pool_(pool) {}

    // Register a task; returns its ID.
    template <typename F>
    TaskId add_task(const std::string& name, F&& f) {
        std::lock_guard lock(mutex_);
        TaskId id = nodes_.size();
        nodes_.push_back(std::make_shared<Node>(id, name, std::forward<F>(f)));
        return id;
    }

    // "successor depends on predecessor": predecessor must finish before successor.
    void add_dependency(TaskId successor, TaskId predecessor);

    // Validate (no cycles) and dispatch ready tasks to the thread pool.
    void run();

    // Block until all tasks in this graph complete.
    void wait();

    // Reset graph to allow re-running with same structure.
    void reset();

private:
    struct Node {
        TaskId                          id;
        std::string                     name;
        std::function<void()>           fn;
        std::vector<TaskId>             successors;   // tasks that depend on me
        std::atomic<int>                pending_deps{0};

        template <typename F>
        Node(TaskId i, std::string n, F&& f)
            : id(i), name(std::move(n)), fn(std::forward<F>(f)) {}
    };

    void check_for_cycles() const;
    void dispatch(std::shared_ptr<Node> node);
    void on_node_complete(std::shared_ptr<Node> node);

    ThreadPool&                             pool_;
    mutable std::mutex                      mutex_;
    std::vector<std::shared_ptr<Node>>      nodes_;

    std::atomic<int>                        remaining_{0};
    std::mutex                              done_mutex_;
    std::condition_variable                 done_cv_;
};
