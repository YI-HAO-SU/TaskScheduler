# TaskScheduler

A C++20 multithreaded task scheduler featuring a lock-free work-stealing thread pool and DAG-based dependency scheduling.

## Architecture

```
                        ┌──────────────────────────────────┐
  submit(f) ──────────► │          Thread Pool             │
                        │                                  │
                        │  Worker 0   Worker 1   Worker N  │
                        │  ┌──────┐  ┌──────┐  ┌──────┐    │
                        │  │deque │  │deque │  │deque │    │
                        │  └──────┘  └──────┘  └──────┘    │
                        │      ▲         │                 │
                        │      └─ steal ─┘   (Chase-Lev)   │
                        │                                  │
                        │       [global queue]             │
                        └──────────────────────────────────┘

  TaskGraph:
    add_task("A", fn)          A ──┐
    add_task("B", fn)              ├──► C ──► E
    add_task("C", fn)          B ──┘          │
    add_dependency(C, A)                      ▼
    add_dependency(C, B)       D ───────────► F
    graph.run()
```

## Features

| Feature | Implementation |
|---------|---------------|
| **Lock-free work stealing** | Chase-Lev deque with `std::atomic` CAS |
| **Future / Promise** | `std::packaged_task` + `std::future<T>` |
| **DAG scheduling** | Atomic ref-count per node; zero-dep → auto dispatch |
| **Cycle detection** | Iterative DFS at `run()` time |
| **Type-erased Task** | Move-only wrapper, no `std::function` overhead |
| **C++20** | `std::jthread`-ready, fold expressions, concepts-friendly |

## Quick Start

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run

```bash
./build/example    # demos: future, work stealing, DAG
./build/tests      # unit tests
./build/benchmark  # throughput / latency / parallel sum
```

## Usage

### Thread Pool + Future

```cpp
#include "thread_pool.hpp"

ThreadPool pool(std::thread::hardware_concurrency());

// Submit any callable; get a typed future back
auto fut = pool.submit([](int x) { return x * x; }, 7);
int result = fut.get();  // 49

// Wait for all submitted tasks to finish
pool.wait_all();
```

### DAG Scheduling

```cpp
#include "task_graph.hpp"

TaskGraph graph(pool);

auto A = graph.add_task("A", [] { /* fetch data   */ });
auto B = graph.add_task("B", [] { /* fetch config */ });
auto C = graph.add_task("C", [] { /* merge A + B  */ });
auto D = graph.add_task("D", [] { /* write output */ });

graph.add_dependency(C, A);   // C runs after A
graph.add_dependency(C, B);   // C runs after B
graph.add_dependency(D, C);   // D runs after C

graph.run();   // A and B execute in parallel
graph.wait();  // block until D completes
```

## Benchmark Results

### Apple M2 (8 threads)

```
[throughput]    100,000 tasks    →  430,000 tasks/s
[latency]       p50 = 4 µs       p99 = 10 µs
[parallel sum]  16M elements     →  1.23x speedup vs serial
```

### Windows 11, x86-64 (12 logical threads / 6 physical cores, MinGW GCC 15.2)

| Benchmark | Parallel | Serial | Speedup | Bottleneck |
|-----------|----------|--------|---------|------------|
| Throughput (100K empty tasks) | — | — | ~1.2 µs/task | Scheduler overhead baseline |
| Latency (submit→result) | — | — | p50 = 13 µs / p99 = 21 µs | OS thread wakeup |
| Parallel sum (16M integers) | 1.4 ms | 7.1 ms | **5.6x** | DRAM bandwidth saturated |
| Trig sin×cos (10M values) | 20.4 ms | 113.0 ms | **5.6x** | 6 physical cores + shared UCRT math tables |

> Speedups are below the logical thread count (12) because every workload has a shared-memory bottleneck. A purely register-bound workload (zero shared reads) would approach the physical core ceiling (~6x).

## Design Notes

### Lock-Free Work-Stealing Deque (Chase-Lev)

Each worker owns a circular deque. The owner pushes/pops from the **bottom** (LIFO, cache-friendly). Thieves steal from the **top** (FIFO) using a single CAS. Memory ordering is carefully chosen per operation:

- `bottom_`: only written by owner → `relaxed` store, `seq_cst` fence before last-item check
- `top_`: written by multiple thieves → `acq_rel` CAS
- Buffer pointer: published with `release`, read by thieves with `consume`

### DAG Scheduler

Each graph node holds an `std::atomic<int> pending_deps` (in-degree). When a task completes, it atomically decrements each successor's counter. A successor whose counter reaches zero is immediately dispatched to the thread pool — no central scheduler lock required.

### Memory Ordering Rationale

| Location | Order | Reason |
|----------|-------|--------|
| `push` bottom store | `relaxed` | Only owner writes |
| `pop` fence | `seq_cst` | Prevent reordering with top load |
| `steal` CAS | `seq_cst` | Total order needed across thieves |
| `pending_deps` decrement | `acq_rel` | Synchronise task completion |

## Project Structure

```
include/
  task.hpp                 # Type-erased Task + make_task<F>()
  work_stealing_queue.hpp  # Lock-free Chase-Lev deque
  thread_pool.hpp          # Thread pool interface
  task_graph.hpp           # DAG scheduler interface
src/
  thread_pool.cpp          # Worker loop, stealing, wait_all
  task_graph.cpp           # DFS cycle check, node dispatch
tests/
  test_all.cpp             # 10 concurrency unit tests
benchmarks/
  benchmark.cpp            # Throughput / latency / parallel sum
examples/
  main.cpp                 # Runnable demos
```

## Requirements

- C++20 compiler (GCC 11+ / Clang 13+ / MSVC 19.29+)
- CMake 3.20+
- pthreads
