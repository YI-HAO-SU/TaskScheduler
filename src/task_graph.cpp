#include "task_graph.hpp"

#include <stdexcept>
#include <stack>

void TaskGraph::add_dependency(TaskId successor, TaskId predecessor) {
    std::lock_guard lock(mutex_);
    if (successor >= nodes_.size() || predecessor >= nodes_.size())
        throw std::out_of_range("TaskGraph::add_dependency: invalid TaskId");

    nodes_[predecessor]->successors.push_back(successor);
    nodes_[successor]->pending_deps.fetch_add(1, std::memory_order_relaxed);
}

void TaskGraph::check_for_cycles() const {
    // Iterative DFS — color: 0=unvisited, 1=in-stack, 2=done
    const std::size_t n = nodes_.size();
    std::vector<int> color(n, 0);

    for (std::size_t start = 0; start < n; ++start) {
        if (color[start] != 0) continue;

        std::stack<std::pair<std::size_t, std::size_t>> stk; // (node, successor_idx)
        stk.push({start, 0});
        color[start] = 1;

        while (!stk.empty()) {
            auto& [node, idx] = stk.top();
            if (idx < nodes_[node]->successors.size()) {
                std::size_t next = nodes_[node]->successors[idx++];
                if (color[next] == 1)
                    throw std::runtime_error(
                        "TaskGraph: cycle detected involving task \"" +
                        nodes_[next]->name + "\"");
                if (color[next] == 0) {
                    color[next] = 1;
                    stk.push({next, 0});
                }
            } else {
                color[node] = 2;
                stk.pop();
            }
        }
    }
}

void TaskGraph::run() {
    std::lock_guard lock(mutex_);
    check_for_cycles();

    remaining_.store(static_cast<int>(nodes_.size()), std::memory_order_relaxed);

    // Dispatch all tasks with no pending dependencies (in-degree == 0).
    for (auto& node : nodes_) {
        if (node->pending_deps.load(std::memory_order_relaxed) == 0)
            dispatch(node);
    }
}

void TaskGraph::dispatch(std::shared_ptr<Node> node) {
    pool_.submit_task(Task([this, node = std::move(node)]() mutable {
        node->fn();
        on_node_complete(std::move(node));
    }));
}

void TaskGraph::on_node_complete(std::shared_ptr<Node> node) {
    // Decrement pending_deps for each successor; dispatch those that become ready.
    for (TaskId sid : node->successors) {
        auto& successor = nodes_[sid];
        int prev = successor->pending_deps.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1)
            dispatch(successor);
    }

    int prev = remaining_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1)
        done_cv_.notify_all();
}

void TaskGraph::wait() {
    std::unique_lock lock(done_mutex_);
    done_cv_.wait(lock, [&] {
        return remaining_.load(std::memory_order_acquire) == 0;
    });
}

void TaskGraph::reset() {
    std::lock_guard lock(mutex_);
    for (auto& node : nodes_)
        node->pending_deps.store(0, std::memory_order_relaxed);
    remaining_.store(0, std::memory_order_relaxed);
}
