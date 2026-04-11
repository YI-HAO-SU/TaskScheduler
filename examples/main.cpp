#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "task_graph.hpp"
#include "thread_pool.hpp"

// ── Demo 1: basic future/promise ─────────────────────────────────────────────
void demo_futures(ThreadPool& pool) {
    std::cout << "\n=== Demo 1: Future / Promise ===\n";

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * i));
            return i * i;
        }));
    }

    for (auto& f : futures)
        std::cout << "  result: " << f.get() << "\n";
}

// ── Demo 2: work stealing ─────────────────────────────────────────────────────
void demo_work_stealing(ThreadPool& pool) {
    std::cout << "\n=== Demo 2: Work Stealing ===\n";

    constexpr int N = 100;
    std::atomic<int> counter{0};

    for (int i = 0; i < N; ++i)
        pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });

    pool.wait_all();
    std::cout << "  completed " << counter.load() << " / " << N << " tasks\n";
}

// ── Demo 3: DAG scheduling ────────────────────────────────────────────────────
//
//   A ──┐
//       ├──> C ──> E
//   B ──┘         |
//                 v
//   D ────────> F (final)
//
void demo_dag(ThreadPool& pool) {
    std::cout << "\n=== Demo 3: DAG Scheduling ===\n";

    TaskGraph graph(pool);

    auto log = [](const char* name) {
        std::cout << "  [task " << name << "] running on thread "
                  << std::this_thread::get_id() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    };

    auto A = graph.add_task("A", [&]{ log("A"); });
    auto B = graph.add_task("B", [&]{ log("B"); });
    auto C = graph.add_task("C", [&]{ log("C"); });
    auto D = graph.add_task("D", [&]{ log("D"); });
    auto E = graph.add_task("E", [&]{ log("E"); });
    auto F = graph.add_task("F", [&]{ log("F"); });

    graph.add_dependency(C, A);   // C needs A
    graph.add_dependency(C, B);   // C needs B
    graph.add_dependency(E, C);   // E needs C
    graph.add_dependency(F, E);   // F needs E
    graph.add_dependency(F, D);   // F needs D

    auto t0 = std::chrono::steady_clock::now();
    graph.run();
    graph.wait();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "  DAG finished in " << ms << " ms "
              << "(serial would be ~120 ms)\n";
}

int main() {
    ThreadPool pool(std::thread::hardware_concurrency());
    std::cout << "Thread pool size: " << pool.thread_count() << "\n";

    demo_futures(pool);
    demo_work_stealing(pool);
    demo_dag(pool);

    std::cout << "\nAll demos complete.\n";
    return 0;
}
