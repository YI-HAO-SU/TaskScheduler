#!/usr/bin/env python3
"""
Compile and run benchmarks, save results to JSON, then draw two charts.

Usage:
    python benchmarks/run_benchmarks.py              # compile + run + plot
    python benchmarks/run_benchmarks.py --skip-compile
    python benchmarks/run_benchmarks.py --plot-only  # plot from saved JSON

Requires: pip install matplotlib numpy
"""
import os, sys, subprocess, multiprocessing, json, argparse

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("Missing: pip install matplotlib numpy")
    sys.exit(1)

# ── Paths ─────────────────────────────────────────────────────────────────────
WORKDIR   = os.path.normpath(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
GPP       = r"C:\msys64\ucrt64\bin\g++.exe"
INC       = os.path.join(WORKDIR, "include")
CFLAGS    = ["-std=c++20", "-O2", "-Wall", "-Wextra"]
MSYS2_BIN = r"C:\msys64\ucrt64\bin"
_ENV      = {**os.environ, "PATH": MSYS2_BIN + os.pathsep + os.environ.get("PATH", "")}
DATA_JSON = os.path.join(WORKDIR, "benchmarks", "benchmark_data.json")

# ── Benchmark descriptors ─────────────────────────────────────────────────────
METRICS = [
    # (bench,          metric,          display title,                  y-label,       higher-is-better)
    ("throughput",    "tasks_per_sec",  "Throughput",                   "tasks/sec",   True ),
    ("latency",       "avg_ms",         "Latency avg  (lower=better)",  "ms",          False),
    ("latency",       "p99_ms",         "Latency p99  (lower=better)",  "ms",          False),
    ("parallel_sum",  "speedup",        "Parallel Sum speedup",         "x vs serial", True ),
    ("trig",          "speedup",        "Trig speedup",                 "x vs serial", True ),
]

PALETTE = {"original": "#e74c3c", "fixed": "#3498db"}

EXES = {
    "fixed":    ("bench_fixed_run.exe",
                 ["src/thread_pool.cpp", "benchmarks/benchmark.cpp"]),
    "original": ("bench_original_run.exe",
                 ["src/thread_pool_baseline.cpp", "benchmarks/benchmark_baseline.cpp"]),
}


# ── Build ─────────────────────────────────────────────────────────────────────

def compile_bench(impl: str) -> None:
    exe, sources = EXES[impl]
    out = os.path.join(WORKDIR, exe)
    cmd = [GPP] + CFLAGS + ["-I", INC]
    cmd += [os.path.join(WORKDIR, s) for s in sources]
    cmd += ["-o", out, "-lpthread"]
    print(f"  [{impl}] compiling ...", end=" ", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, env=_ENV)
    if r.returncode != 0:
        print("FAILED\n" + r.stderr)
        sys.exit(1)
    print("OK")


# ── Run one scenario ──────────────────────────────────────────────────────────

def run_one(impl: str, threads: int, timeout_s: int = 170) -> list:
    exe  = EXES[impl][0]
    path = os.path.join(WORKDIR, exe)
    try:
        r = subprocess.run(
            [path, str(threads)],
            capture_output=True, text=True,
            cwd=WORKDIR, env=_ENV, timeout=timeout_s,
        )
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT after {timeout_s}s — skipping")
        return []

    if r.returncode != 0:
        print(f"  ERROR rc={r.returncode}: {r.stderr[:300]}")
        return []

    for line in r.stdout.splitlines():
        if not line.startswith("CSV,"):
            print("   ", line)

    rows = []
    for line in r.stdout.splitlines():
        if not line.startswith("CSV,"):
            continue
        parts = line.split(",")
        if len(parts) == 6:
            _, impl2, t, bench, metric, val = parts
            rows.append({"impl": impl2, "threads": int(t),
                         "bench": bench, "metric": metric, "value": float(val)})
    return rows


# ── Collect all scenarios and save JSON ───────────────────────────────────────

def collect(thread_counts: list, hw: int) -> dict:
    records = []
    for nth in thread_counts:
        for impl in ["original", "fixed"]:
            sep = "-" * 52
            print(f"\n{sep}\n  {impl:10s}  |  {nth} threads\n{sep}")
            records.extend(run_one(impl, nth))

    result = {"hw": hw, "thread_counts": thread_counts, "records": records}
    with open(DATA_JSON, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nResults saved -> {DATA_JSON}")
    return result


# ── Load JSON into a lookup dict ──────────────────────────────────────────────

def load_saved(saved: dict):
    hw           = saved["hw"]
    thread_counts = saved["thread_counts"]
    data = {}
    for r in saved["records"]:
        key = (r["impl"], r["threads"], r["bench"], r["metric"])
        data[key] = r["value"]
    return data, thread_counts, hw


# ── Plot helpers ──────────────────────────────────────────────────────────────

def fmt_val(v: float) -> str:
    if v >= 1e6:  return f"{v/1e6:.1f}M"
    if v >= 1e3:  return f"{v/1e3:.0f}k"
    if v < 0.01:  return f"{v:.4f}"
    if v < 1:     return f"{v:.3f}"
    return f"{v:.2f}"


def xlabels(thread_counts: list, hw: int) -> list:
    out = []
    for t in thread_counts:
        if t == 1:   out.append("1T\n(serial)")
        elif t == hw: out.append(f"{t}T\n(all)")
        else:         out.append(f"{t}T")
    return out


def save_show(fig, filename: str) -> None:
    path = os.path.join(WORKDIR, "benchmarks", filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    print(f"  Saved -> {path}")
    try:
        plt.show()
    except Exception:
        pass


# ── Figure 1: grouped bar chart (original vs fixed, each thread count) ────────

def plot_bar(data: dict, thread_counts: list, hw: int) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(17, 9))
    fig.suptitle(
        f"Original (w/ issues)  vs  Fixed  |  hw = {hw} cores",
        fontsize=13, fontweight="bold", y=0.99,
    )
    axes_flat = axes.flatten()
    x  = np.arange(len(thread_counts))
    xl = xlabels(thread_counts, hw)
    w  = 0.35

    for i, (bench, metric, title, ylabel, _) in enumerate(METRICS):
        ax = axes_flat[i]
        for j, impl in enumerate(["original", "fixed"]):
            vals   = [data.get((impl, t, bench, metric), 0.0) for t in thread_counts]
            offset = (j - 0.5) * w
            bars   = ax.bar(x + offset, vals, w,
                            label=impl.capitalize(),
                            color=PALETTE[impl], edgecolor="white",
                            linewidth=0.5, alpha=0.88)
            for bar, v in zip(bars, vals):
                if v > 0:
                    ax.text(bar.get_x() + bar.get_width() / 2,
                            bar.get_height() * 1.02, fmt_val(v),
                            ha="center", va="bottom", fontsize=6.5)
        ax.set_title(title, fontsize=10, fontweight="semibold")
        ax.set_ylabel(ylabel, fontsize=9)
        ax.set_xticks(x)
        ax.set_xticklabels(xl, fontsize=8)
        ax.legend(fontsize=8, loc="upper left")
        ax.grid(axis="y", alpha=0.3, linewidth=0.5)
        ax.set_axisbelow(True)

    axes_flat[5].axis("off")
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    save_show(fig, "benchmark_results.png")


# ── Figure 2: scaling line chart (both impls, 1→all threads) ──────────────────

def plot_scaling(data: dict, thread_counts: list, hw: int) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(17, 9))
    fig.suptitle(
        f"Scaling curves  —  Original vs Fixed  |  hw = {hw} cores",
        fontsize=13, fontweight="bold", y=0.99,
    )
    axes_flat = axes.flatten()
    xl = xlabels(thread_counts, hw)

    style = {
        "original": dict(color=PALETTE["original"], marker="o", linestyle="--",
                         linewidth=1.8, markersize=7, alpha=0.85, label="Original"),
        "fixed":    dict(color=PALETTE["fixed"],    marker="s", linestyle="-",
                         linewidth=2.2, markersize=7, alpha=0.9,  label="Fixed"),
    }

    for i, (bench, metric, title, ylabel, higher) in enumerate(METRICS):
        ax = axes_flat[i]

        for impl in ["original", "fixed"]:
            ys_all = [data.get((impl, t, bench, metric)) for t in thread_counts]
            xs = [t for t, v in zip(thread_counts, ys_all) if v is not None]
            ys = [v for v in ys_all if v is not None]
            if not xs:
                continue
            ax.plot(xs, ys, **style[impl])
            for xp, yp in zip(xs, ys):
                ax.annotate(fmt_val(yp), xy=(xp, yp),
                            xytext=(0, 5 if higher else -8),
                            textcoords="offset points",
                            ha="center",
                            va="bottom" if higher else "top",
                            fontsize=7)

        # Ideal-linear reference for speedup metrics (from 1T fixed point)
        if bench in ("parallel_sum", "trig") and metric == "speedup":
            base = data.get(("fixed", thread_counts[0], bench, metric))
            if base is not None:
                ideal = [base * (t / thread_counts[0]) for t in thread_counts]
                ax.plot(thread_counts, ideal, color="gray", linestyle=":",
                        linewidth=1.2, alpha=0.6, label="Ideal linear")

        ax.set_title(title, fontsize=10, fontweight="semibold")
        ax.set_ylabel(ylabel, fontsize=9)
        ax.set_xlabel("Thread count", fontsize=8)
        ax.set_xticks(thread_counts)
        ax.set_xticklabels(xl, fontsize=8)
        ax.legend(fontsize=8, loc="upper left")
        ax.grid(alpha=0.25, linewidth=0.5)
        ax.set_axisbelow(True)

    axes_flat[5].axis("off")
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    save_show(fig, "benchmark_scaling.png")


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skip-compile", action="store_true",
                        help="skip compilation, reuse existing binaries")
    parser.add_argument("--plot-only", action="store_true",
                        help=f"load {DATA_JSON} and plot without running benchmarks")
    args = parser.parse_args()

    if args.plot_only:
        if not os.path.exists(DATA_JSON):
            print(f"No data file: {DATA_JSON}\nRun without --plot-only first.")
            sys.exit(1)
        with open(DATA_JSON) as f:
            saved = json.load(f)
        print(f"Loaded saved results from {DATA_JSON}")
    else:
        hw = multiprocessing.cpu_count()
        seen: set = set()
        thread_counts: list = []
        for t in [1, 2, 4, 8, hw]:
            if t not in seen:
                seen.add(t)
                thread_counts.append(t)

        print(f"Hardware threads : {hw}")
        print(f"Thread scenarios : {thread_counts}")
        print(f"Timeout per run  : 170s\n")

        if not args.skip_compile:
            print("Building benchmarks:")
            compile_bench("fixed")
            compile_bench("original")
            print()

        saved = collect(thread_counts, hw)

    data, thread_counts, hw = load_saved(saved)

    if not data:
        print("No data to plot.")
        sys.exit(1)

    print("\nRendering Figure 1: bar chart ...")
    plot_bar(data, thread_counts, hw)

    print("Rendering Figure 2: scaling line chart ...")
    plot_scaling(data, thread_counts, hw)

    print("\nDone.")


if __name__ == "__main__":
    main()
