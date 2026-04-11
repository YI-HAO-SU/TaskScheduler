#pragma once

#include <functional>
#include <future>
#include <memory>
#include <type_traits>

// A type-erased, movable task wrapper that supports std::future<T> returns.
// Internally uses std::packaged_task so the caller can wait on the result.
class Task {
public:
    Task() = default;

    template <typename F>
    explicit Task(F&& f)
        : impl_(std::make_unique<ConcreteTask<std::decay_t<F>>>(std::forward<F>(f)))
    {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    void operator()() { impl_->invoke(); }
    bool valid() const { return impl_ != nullptr; }

private:
    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual void invoke() = 0;
    };

    template <typename F>
    struct ConcreteTask : TaskBase {
        F fn;
        explicit ConcreteTask(F&& f) : fn(std::move(f)) {}
        void invoke() override { fn(); }
    };

    std::unique_ptr<TaskBase> impl_;
};

// Helper: wrap a callable into a Task and return a future for its result.
// Usage:
//   auto [task, fut] = make_task([]{ return 42; });
//   pool.submit(std::move(task));
//   int result = fut.get();
template <typename F>
auto make_task(F&& f)
    -> std::pair<Task, std::future<std::invoke_result_t<std::decay_t<F>>>>
{
    using R = std::invoke_result_t<std::decay_t<F>>;
    auto pt  = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
    auto fut = pt->get_future();
    Task task([pt = std::move(pt)]() mutable { (*pt)(); });
    return {std::move(task), std::move(fut)};
}
