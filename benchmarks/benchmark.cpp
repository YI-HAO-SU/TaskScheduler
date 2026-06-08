#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "thread_pool.hpp"

using Clock = std::chrono::steady_clock;

static double elapsed_ms(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void emit_csv(const std::string& impl, int nth,
                     const char* bench, const char* metric, double val) {
    std::cout << "CSV," << impl << "," << nth << ","
              << bench << "," << metric << "," << val << "\n";
}

// ── Benchmark 1: throughput (empty tasks) ────────────────────────────────────
void bench_throughput(ThreadPool& pool, int n_tasks,
                      const std::string& impl, int nth) {
    std::atomic<int> count{0};
    auto t0 = Clock::now();

    for (int i = 0; i < n_tasks; ++i)
        pool.submit([&count]{ count.fetch_add(1, std::memory_order_relaxed); });

    pool.wait_all();
    double ms  = elapsed_ms(t0);
    auto   tps = static_cast<long long>(n_tasks / ms * 1000);
    std::cout << "[throughput]  " << n_tasks << " tasks in "
              << ms << " ms  (" << tps << " tasks/s)\n";
    emit_csv(impl, nth, "throughput", "tasks_per_sec", static_cast<double>(tps));
}

// ── Benchmark 2: latency (submit → future.get) ───────────────────────────────
void bench_latency(ThreadPool& pool, int n_samples,
                   const std::string& impl, int nth) {
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
    double p50 = latencies[n_samples / 2];
    double p99 = latencies[n_samples * 99 / 100];
    std::cout << "[latency]     avg=" << avg << " ms  "
              << "p50=" << p50 << " ms  "
              << "p99=" << p99 << " ms\n";
    emit_csv(impl, nth, "latency", "avg_ms", avg);
    emit_csv(impl, nth, "latency", "p50_ms", p50);
    emit_csv(impl, nth, "latency", "p99_ms", p99);
}

// ── Benchmark 3: parallel sum (CPU-bound work) ───────────────────────────────
void bench_parallel_sum(ThreadPool& pool, const std::string& impl, int nth) {
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

    auto t1 = Clock::now();
    long long serial = 0;
    for (int x : data) serial += x;
    double ms_serial = elapsed_ms(t1);

    double speedup = ms_serial / ms;
    std::cout << "[parallel sum] sum=" << total << " in " << ms << " ms\n"
              << "[serial   sum] sum=" << serial << " in " << ms_serial
              << " ms  (speedup: " << speedup << "x)\n";
    emit_csv(impl, nth, "parallel_sum", "speedup", speedup);
    emit_csv(impl, nth, "parallel_sum", "par_ms",  ms);
}

// ── Benchmark 4: CPU-bound trig (embarrassingly parallel) ────────────────────
void bench_trig(ThreadPool& pool, const std::string& impl, int nth) {
    constexpr int N   = 10'000'000;
    const int nthreads = static_cast<int>(pool.thread_count());
    const int chunk    = N / nthreads;

    auto t0 = Clock::now();
    std::vector<std::future<double>> futs;
    futs.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        int lo = t * chunk;
        int hi = (t == nthreads - 1) ? N : lo + chunk;
        futs.push_back(pool.submit([lo, hi] {
            double s = 0.0;
            for (int i = lo; i < hi; ++i)
                s += std::sin(static_cast<double>(i)) *
                     std::cos(static_cast<double>(i));
            return s;
        }));
    }
    double par_sum = 0.0;
    for (auto& f : futs) par_sum += f.get();
    double ms_par = elapsed_ms(t0);

    auto t1 = Clock::now();
    double ser_sum = 0.0;
    for (int i = 0; i < N; ++i)
        ser_sum += std::sin(static_cast<double>(i)) *
                   std::cos(static_cast<double>(i));
    double ms_ser = elapsed_ms(t1);

    bool   ok      = std::abs(par_sum - ser_sum) < std::abs(ser_sum) * 1e-6 + 1e-9;
    double speedup = ms_ser / ms_par;
    std::cout << "[trig par]    10M sin*cos in " << ms_par << " ms\n"
              << "[trig ser]    10M sin*cos in " << ms_ser << " ms  "
              << "(speedup: " << speedup << "x)"
              << (ok ? "" : "  [SUM MISMATCH]") << "\n";
    emit_csv(impl, nth, "trig", "speedup", speedup);
    emit_csv(impl, nth, "trig", "par_ms",  ms_par);
}

int main(int argc, char* argv[]) {
    std::size_t nthreads = std::thread::hardware_concurrency();
    if (argc > 1) {
        int n = std::stoi(argv[1]);
        if (n > 0) nthreads = static_cast<std::size_t>(n);
    }

    const std::string impl = "fixed";
    const int         nth  = static_cast<int>(nthreads);
    ThreadPool pool(nthreads);
    std::cout << "=== " << impl << " | threads=" << nth << " ===\n\n" << std::flush;

    bench_throughput(pool, 100'000, impl, nth);
    std::cout << std::flush;
    bench_latency(pool, 1'000, impl, nth);
    std::cout << std::flush;
    bench_parallel_sum(pool, impl, nth);
    std::cout << "\n" << std::flush;
    bench_trig(pool, impl, nth);
    std::cout << std::flush;

    return 0;
}
