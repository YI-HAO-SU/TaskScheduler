#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <numeric>
#include <vector>

#include "thread_pool.hpp"

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

static double elapsed_ms(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ── Benchmark 1: throughput (empty tasks) ────────────────────────────────────
void bench_throughput(ThreadPool& pool, int n_tasks) {
    std::atomic<int> count{0};
    auto t0 = Clock::now();

    for (int i = 0; i < n_tasks; ++i)
        pool.submit([&count]{ count.fetch_add(1, std::memory_order_relaxed); });

    pool.wait_all();
    double ms = elapsed_ms(t0);
    std::cout << "[throughput]  " << n_tasks << " tasks in "
              << ms << " ms  ("
              << static_cast<int>(n_tasks / ms * 1000) << " tasks/s)\n";
}

// ── Benchmark 2: latency (submit → future.get) ───────────────────────────────
void bench_latency(ThreadPool& pool, int n_samples) {
    std::vector<double> latencies;
    latencies.reserve(n_samples);

    for (int i = 0; i < n_samples; ++i) {
        auto t0  = Clock::now();
        auto fut = pool.submit([]{ return 42; });
        fut.get();
        latencies.push_back(elapsed_ms(t0));
    }

    std::sort(latencies.begin(), latencies.end());
    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / n_samples;
    std::cout << "[latency]     avg=" << avg << " ms  "
              << "p50=" << latencies[n_samples / 2] << " ms  "
              << "p99=" << latencies[n_samples * 99 / 100] << " ms\n";
}

// ── Benchmark 3: parallel sum (CPU-bound work) ───────────────────────────────
void bench_parallel_sum(ThreadPool& pool) {
    constexpr int N      = 1 << 24;  // 16 M elements
    constexpr int CHUNKS = 16;

    std::vector<int> data(N);
    std::iota(data.begin(), data.end(), 0);

    auto t0 = Clock::now();

    std::vector<std::future<long long>> futs;
    futs.reserve(CHUNKS);
    int chunk = N / CHUNKS;

    for (int c = 0; c < CHUNKS; ++c) {
        int lo = c * chunk;
        int hi = (c == CHUNKS - 1) ? N : lo + chunk;
        futs.push_back(pool.submit([&data, lo, hi]() -> long long {
            long long s = 0;
            for (int i = lo; i < hi; ++i) s += data[i];
            return s;
        }));
    }

    long long total = 0;
    for (auto& f : futs) total += f.get();

    double ms = elapsed_ms(t0);
    std::cout << "[parallel sum] sum=" << total
              << " in " << ms << " ms\n";

    // Serial reference
    auto t1 = Clock::now();
    long long serial = 0;
    for (int x : data) serial += x;
    double ms_serial = elapsed_ms(t1);
    std::cout << "[serial   sum] sum=" << serial
              << " in " << ms_serial << " ms  "
              << "(speedup: " << ms_serial / ms << "x)\n";
}

int main() {
    const std::size_t nthreads = std::thread::hardware_concurrency();
    ThreadPool pool(nthreads);
    std::cout << "Threads: " << nthreads << "\n\n";

    bench_throughput(pool, 100'000);
    bench_latency(pool, 1'000);
    bench_parallel_sum(pool);

    return 0;
}
