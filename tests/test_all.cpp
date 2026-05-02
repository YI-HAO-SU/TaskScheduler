#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "task_graph.hpp"
#include "thread_pool.hpp"

// Minimal test harness (no external dependencies)
static int g_pass = 0, g_fail = 0;

#define EXPECT(cond)                                                 \
    do {                                                             \
        if (!(cond)) {                                               \
            std::cerr << "  FAIL  " << #cond                        \
                      << "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_fail;                                                \
        } else {                                                     \
            ++g_pass;                                                \
        }                                                            \
    } while (0)

static constexpr int TIMEOUT_SECS = 15;

// Runs fn() in a detached thread watched by a 15-second deadline.
// On timeout, prints a message and calls quick_exit(2) so the retry
// script can detect a deadlock and re-launch the whole suite.
static void run_test(const char* name, void (*fn)()) {
    std::atomic<bool> done{false};
    std::thread([fn, &done]() {
        fn();
        done.store(true, std::memory_order_release);
    }).detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(TIMEOUT_SECS);
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "\nTIMEOUT (" << TIMEOUT_SECS << "s) in " << name
                      << " — likely deadlock\n" << std::flush;
            std::quick_exit(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ── Thread pool ───────────────────────────────────────────────────────────────

void test_submit_future() {
    std::cout << "test_submit_future\n";
    ThreadPool pool(2);
    auto fut = pool.submit([]{ return 42; });
    EXPECT(fut.get() == 42);
}

void test_submit_many() {
    std::cout << "test_submit_many\n";
    ThreadPool pool(4);
    constexpr int N = 1000;
    std::atomic<int> counter{0};

    for (int i = 0; i < N; ++i)
        pool.submit([&counter]{ counter.fetch_add(1, std::memory_order_relaxed); });

    pool.wait_all();
    EXPECT(counter.load() == N);
}

void test_submit_with_args() {
    std::cout << "test_submit_with_args\n";
    ThreadPool pool(2);
    auto fut = pool.submit([](int a, int b){ return a + b; }, 10, 32);
    EXPECT(fut.get() == 42);
}

void test_exception_propagation() {
    std::cout << "test_exception_propagation\n";
    ThreadPool pool(2);
    auto fut = pool.submit([]() -> int { throw std::runtime_error("oops"); });
    bool caught = false;
    try { fut.get(); }
    catch (const std::runtime_error& e) { caught = true; }
    EXPECT(caught);
}

// ── Work stealing queue ───────────────────────────────────────────────────────

void test_work_stealing_concurrent() {
    std::cout << "test_work_stealing_concurrent\n";
    // Submit uneven workload to force stealing.
    ThreadPool pool(4);
    constexpr int N = 500;
    std::atomic<int> counter{0};

    // Batch-push to one worker's internal queue via nested submits.
    pool.submit([&pool, &counter] {
        for (int i = 0; i < N; ++i)
            pool.submit([&counter]{
                counter.fetch_add(1, std::memory_order_relaxed);
            });
    }).get();   // wait for outer

    pool.wait_all();
    EXPECT(counter.load() == N);
}

// ── Task graph ────────────────────────────────────────────────────────────────

void test_dag_ordering() {
    std::cout << "test_dag_ordering\n";
    ThreadPool pool(4);
    TaskGraph graph(pool);

    std::vector<int> order;
    std::mutex mu;

    // A → C, B → C, C → D
    auto A = graph.add_task("A", [&]{ std::lock_guard l(mu); order.push_back(0); });
    auto B = graph.add_task("B", [&]{ std::lock_guard l(mu); order.push_back(1); });
    auto C = graph.add_task("C", [&]{ std::lock_guard l(mu); order.push_back(2); });
    auto D = graph.add_task("D", [&]{ std::lock_guard l(mu); order.push_back(3); });

    graph.add_dependency(C, A);
    graph.add_dependency(C, B);
    graph.add_dependency(D, C);

    graph.run();
    graph.wait();

    // D must be last, C must come before D
    EXPECT(order.size() == 4);
    EXPECT(order.back() == 3);   // D is last
    int c_pos = -1, d_pos = -1;
    for (int i = 0; i < (int)order.size(); ++i) {
        if (order[i] == 2) c_pos = i;
        if (order[i] == 3) d_pos = i;
    }
    EXPECT(c_pos < d_pos);
}

void test_dag_independent_parallel() {
    std::cout << "test_dag_independent_parallel\n";
    ThreadPool pool(4);
    TaskGraph graph(pool);

    // 4 independent tasks — should all run concurrently
    constexpr int SLEEP_MS = 50;
    for (int i = 0; i < 4; ++i)
        graph.add_task("T" + std::to_string(i), []{
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });

    auto t0 = std::chrono::steady_clock::now();
    graph.run();
    graph.wait();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // With 4 threads, 4×50 ms tasks should finish well under 4×50 = 200 ms
    EXPECT(ms < SLEEP_MS * 3);
}

void test_dag_cycle_detection() {
    std::cout << "test_dag_cycle_detection\n";
    ThreadPool pool(2);
    TaskGraph graph(pool);

    auto A = graph.add_task("A", []{});
    auto B = graph.add_task("B", []{});
    graph.add_dependency(A, B);
    graph.add_dependency(B, A);   // cycle

    bool caught = false;
    try { graph.run(); }
    catch (const std::runtime_error&) { caught = true; }
    EXPECT(caught);
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    run_test("test_submit_future",            test_submit_future);
    run_test("test_submit_many",              test_submit_many);
    run_test("test_submit_with_args",         test_submit_with_args);
    run_test("test_exception_propagation",    test_exception_propagation);
    run_test("test_work_stealing_concurrent", test_work_stealing_concurrent);
    run_test("test_dag_ordering",             test_dag_ordering);
    run_test("test_dag_independent_parallel", test_dag_independent_parallel);
    run_test("test_dag_cycle_detection",      test_dag_cycle_detection);

    std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
