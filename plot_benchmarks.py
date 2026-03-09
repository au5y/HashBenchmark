#!/usr/bin/env python3
"""
Plot benchmark results and optionally write a Markdown report.

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
                               --output  benchmark_plot.png \\
                               --report  results.md
"""

import argparse
import json
import sys
from collections import defaultdict
from datetime import datetime, timezone
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

STRATEGY_ORDER = ["PerfectHash", "SortedArray", "StructuredTrie"]


def load_runtime(path: Path) -> tuple[dict, dict]:
    """Parse Google Benchmark JSON → (runtime, sizes) dicts keyed by algo → {n: value}."""
    with open(path) as f:
        data = json.load(f)

    runtime: dict = defaultdict(dict)
    sizes: dict   = defaultdict(dict)

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
    """Parse measure_compile_time.py JSON → {algo: {n: seconds}}."""
    with open(path) as f:
        data = json.load(f)

    result: dict = defaultdict(dict)
    for entry in data:
        result[entry["strategy"]][entry["n"]] = entry["compile_seconds"]
    return result


def sorted_xy(d: dict) -> tuple[list, list]:
    pairs = sorted(d.items())
    return [p[0] for p in pairs], [p[1] for p in pairs]


def fmt_bytes(b: float) -> str:
    if b < 1024:
        return f"{int(b):,} B"
    if b < 1024 ** 2:
        return f"{b / 1024:.1f} KiB"
    return f"{b / 1024**2:.1f} MiB"


def fmt_ns(ns: float) -> str:
    return f"{ns:,.0f}"


def plot_panel(ax, datasets: dict, ylabel: str, title: str,
               formatter=None, ylog: bool = False) -> None:
    for algo in STRATEGY_ORDER:
        series = datasets.get(algo)
        if not series:
            continue
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


def build_md_table(rows_by_n: dict[int, dict[str, str]], col_fmt: str = "") -> str:
    """
    rows_by_n: {n: {strategy: formatted_value}}
    Returns a GitHub-flavoured Markdown table string.
    """
    algos = [a for a in STRATEGY_ORDER if any(a in v for v in rows_by_n.values())]
    header = "| N | " + " | ".join(algos) + " |"
    sep    = "|---|" + "|".join(["---:" for _ in algos]) + "|"
    lines  = [header, sep]
    for n in sorted(rows_by_n):
        cells = [rows_by_n[n].get(a, "—") for a in algos]
        lines.append(f"| {n} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def write_report(report_path: Path, plot_path: Path,
                 runtime_data: dict, size_data: dict, compile_data: dict) -> None:
    ns = sorted({n for series in runtime_data.values() for n in series})

    # --- Runtime table ---
    rt_rows: dict[int, dict[str, str]] = {}
    for n in ns:
        rt_rows[n] = {a: fmt_ns(runtime_data[a][n]) + " ns"
                      for a in STRATEGY_ORDER if n in runtime_data.get(a, {})}
    rt_table = build_md_table(rt_rows)

    # --- Size table ---
    sz_rows: dict[int, dict[str, str]] = {}
    for n in ns:
        sz_rows[n] = {a: fmt_bytes(size_data[a][n])
                      for a in STRATEGY_ORDER if n in size_data.get(a, {})}
    sz_table = build_md_table(sz_rows) if any(sz_rows.values()) else None

    # --- Compile-time table ---
    if compile_data:
        ct_ns = sorted({n for series in compile_data.values() for n in series})
        ct_rows: dict[int, dict[str, str]] = {}
        for n in ct_ns:
            ct_rows[n] = {a: f"{compile_data[a][n]:.2f}s"
                          for a in STRATEGY_ORDER if n in compile_data.get(a, {})}
        ct_table = build_md_table(ct_rows)
    else:
        ct_table = None

    # --- Relative image path from report to plot ---
    try:
        img_ref = plot_path.relative_to(report_path.parent)
    except ValueError:
        img_ref = plot_path  # fallback to absolute if not under same dir

    timestamp = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    lines = [
        "# Dispatch Benchmark Results",
        "",
        f"_Generated {timestamp}_",
        "",
        "Three zero-allocation, compile-time dispatch strategies benchmarked across "
        "increasing numbers of registered message IDs (N).",
        "",
        "---",
        "",
        "## Runtime Access Latency",
        "",
        "CPU time per **10,000 lookups** (mix of hits and misses). Lower is better.",
        "",
        rt_table,
    ]

    if sz_table:
        lines += [
            "",
            "## Container Memory Footprint",
            "",
            "Size of the dispatch table in read-only flash (`.rodata`). "
            "Note: StructuredTrie size is **constant** regardless of N — "
            "it always allocates the full `32 × 2048` pointer array.",
            "",
            sz_table,
        ]

    if ct_table:
        lines += [
            "",
            "## Compile-Time Cost (constexpr construction)",
            "",
            "Seconds the compiler spends building each table, measured by compiling "
            "a minimal program per (strategy, N). Reflects the O(N²) selection sort "
            "in PerfectHash versus O(N log N) for SortedArray.",
            "",
            ct_table,
        ]

    lines += [
        "",
        "---",
        "",
        "## Plots",
        "",
        f"![Benchmark comparison plot]({img_ref})",
        "",
        "---",
        "",
        "## Strategy notes",
        "",
        "| Strategy | Lookup | Build complexity | Flash cost |",
        "|---|---|---|---|",
        "| **PerfectHash** | O(1) — 2 hash evals + 2 reads | O(N²) constexpr | ~16 × N bytes |",
        "| **SortedArray** | O(log N) — binary search | O(N log N) constexpr | ~16 × N bytes |",
        "| **StructuredTrie** | O(1) — 2 array reads + bit-shifts | O(N) constexpr | 512 KiB fixed |",
    ]

    report_path.write_text("\n".join(lines) + "\n")
    print(f"Report saved to {report_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", required=True,
                        help="Google Benchmark JSON output file")
    parser.add_argument("--compile", default=None,
                        help="measure_compile_time.py JSON output (optional)")
    parser.add_argument("--output",  default="benchmark_plot.png",
                        help="Output image file (default: benchmark_plot.png)")
    parser.add_argument("--report",  default=None,
                        help="Write a Markdown summary to this file (e.g. results.md)")
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
            lambda x, _: fmt_bytes(x)
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
    output_path = Path(args.output)
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    print(f"Plot saved to {output_path}")

    if args.report:
        write_report(
            report_path=Path(args.report),
            plot_path=output_path,
            runtime_data=runtime_data,
            size_data=size_data,
            compile_data=compile_data,
        )


if __name__ == "__main__":
    main()
