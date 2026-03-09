#!/usr/bin/env python3
"""
Plot benchmark results for the three dispatch strategies.

Produces a figure with up to three panels:
  1. Runtime  — CPU time per 10k lookups (ns), from Google Benchmark JSON
  2. Size     — Dispatch table size in bytes, from the 'bytes' counter in
                Google Benchmark JSON
  3. Compile  — Constexpr construction time per (strategy, N) in seconds,
                from measure_compile_time.py JSON  [optional]

Usage:
    python3 plot_benchmarks.py --runtime benchmark_results.json
    python3 plot_benchmarks.py --runtime benchmark_results.json \\
                               --compile compile_times.json \\
                               --output  benchmark_plot.png
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # headless backend — no display required
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker


# Consistent colours and markers across all panels
STYLE = {
    "PerfectHash":    {"color": "#e05c2a", "marker": "o"},
    "SortedArray":    {"color": "#2a7ae0", "marker": "s"},
    "StructuredTrie": {"color": "#2aad2a", "marker": "^"},
}


def load_runtime(path: Path) -> dict:
    """Parse Google Benchmark JSON → {algo: {n: (cpu_time, bytes)}}"""
    with open(path) as f:
        data = json.load(f)

    runtime = defaultdict(dict)
    sizes   = defaultdict(dict)

    for bench in data.get("benchmarks", []):
        name = bench.get("name", "")
        if "/" not in name:
            continue
        algo, size_str = name.rsplit("/", 1)
        try:
            n = int(size_str)
        except ValueError:
            continue
        runtime[algo][n] = bench.get("cpu_time", 0)
        if "bytes" in bench:
            sizes[algo][n] = bench["bytes"]

    return runtime, sizes


def load_compile(path: Path) -> dict:
    """Parse measure_compile_time.py JSON → {algo: {n: seconds}}"""
    with open(path) as f:
        data = json.load(f)

    result = defaultdict(dict)
    for entry in data:
        result[entry["strategy"]][entry["n"]] = entry["compile_seconds"]
    return result


def sorted_xy(d: dict) -> tuple[list, list]:
    pairs = sorted(d.items())
    return [p[0] for p in pairs], [p[1] for p in pairs]


def plot_panel(ax, datasets: dict, ylabel: str, title: str,
               formatter=None, ylog: bool = False) -> None:
    for algo, series in sorted(datasets.items()):
        style = STYLE.get(algo, {})
        x, y = sorted_xy(series)
        ax.plot(x, y,
                label=algo,
                color=style.get("color"),
                marker=style.get("marker", "o"),
                linewidth=2,
                markersize=6)
    ax.set_xscale("log")
    if ylog:
        ax.set_yscale("log")
    ax.set_xlabel("Number of registered IDs (N)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend()
    ax.grid(True, which="both", linestyle="--", alpha=0.5)
    if formatter:
        ax.yaxis.set_major_formatter(formatter)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", required=True,
                        help="Google Benchmark JSON output file")
    parser.add_argument("--compile", default=None,
                        help="measure_compile_time.py JSON output (optional)")
    parser.add_argument("--output",  default="benchmark_plot.png",
                        help="Output image file (default: benchmark_plot.png)")
    args = parser.parse_args()

    runtime_data, size_data = load_runtime(Path(args.runtime))
    compile_data = load_compile(Path(args.compile)) if args.compile else {}

    has_sizes   = any(size_data.values())
    has_compile = bool(compile_data)

    n_panels = 1 + int(has_sizes) + int(has_compile)
    fig, axes = plt.subplots(1, n_panels, figsize=(6 * n_panels, 5))
    if n_panels == 1:
        axes = [axes]

    panel = 0

    # --- Panel 1: runtime ---
    plot_panel(
        axes[panel], runtime_data,
        ylabel="CPU time per 10k lookups (ns)",
        title="Runtime Access Latency",
    )
    panel += 1

    # --- Panel 2: container size ---
    if has_sizes:
        bytes_formatter = ticker.FuncFormatter(
            lambda x, _: f"{int(x):,} B" if x < 1024 else
                          f"{x/1024:.1f} KiB" if x < 1024**2 else
                          f"{x/1024**2:.1f} MiB"
        )
        plot_panel(
            axes[panel], size_data,
            ylabel="Dispatch table size",
            title="Container Memory Footprint",
            formatter=bytes_formatter,
            ylog=True,
        )
        panel += 1

    # --- Panel 3: compile time ---
    if has_compile:
        plot_panel(
            axes[panel], compile_data,
            ylabel="Compile time (s)",
            title="Constexpr Construction Cost\n(compile-time)",
        )
        panel += 1

    fig.suptitle("Compile-Time Static Dispatch — Strategy Comparison", fontsize=14, y=1.02)
    plt.tight_layout()
    plt.savefig(args.output, dpi=150, bbox_inches="tight")
    print(f"Plot saved to {args.output}")


if __name__ == "__main__":
    main()
