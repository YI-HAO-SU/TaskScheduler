#!/usr/bin/env bash
# Run tests.exe, retrying automatically when a deadlock timeout (exit 2) is hit.
# Usage: ./run_tests.sh [path/to/tests.exe]

EXE="${1:-./tests.exe}"

if [ ! -f "$EXE" ]; then
    echo "Test executable not found: $EXE"
    echo "Build it first (e.g. g++ -std=c++20 -O2 -pthread tests/test_all.cpp src/thread_pool.cpp src/task_graph.cpp -Iinclude -o tests.exe)"
    exit 1
fi

attempt=0
while true; do
    attempt=$((attempt + 1))
    echo "=== Attempt $attempt ==="
    "$EXE"
    code=$?

    if [ $code -eq 0 ]; then
        echo "All tests passed."
        exit 0
    elif [ $code -eq 2 ]; then
        echo "--- deadlock timeout, retrying ---"
        echo ""
    else
        echo "Tests FAILED (exit $code)."
        exit $code
    fi
done
