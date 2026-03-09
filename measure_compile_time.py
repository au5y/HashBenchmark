#!/usr/bin/env python3
"""
Measure per-(strategy, N) compile time as a proxy for constexpr evaluation cost.

For each combination of strategy and table size N, this script compiles a
minimal program that builds only that strategy's dispatch table, timing how
long the compiler takes. The result is a direct measure of the constexpr
construction cost paid at build time in a real project.

Usage:
    python3 measure_compile_time.py [--base 8] [--steps 3] [--compiler g++] [--output compile_times.json]
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

STRATEGIES = {
    "PerfectHash":   "STRATEGY_PH",
    "SortedArray":   "STRATEGY_SA",
    "StructuredTrie": "STRATEGY_TRIE",
}

# Compile flags must match the benchmark build for a fair comparison.
# -O0 avoids optimizer passes so timing reflects constexpr eval, not codegen.
# -c (compile-only, no link) isolates the constexpr evaluation cost.
BASE_FLAGS = [
    "-std=c++17",
    "-O0",
    "-c",
]


def get_sizes(base: int, steps: int) -> list[int]:
    return [base ** i for i in range(1, steps + 1)]


def compile_and_time(cpp_text: str, include_dir: Path, defines: list[str],
                     extra_flags: list[str], compiler: str) -> float | None:
    """Write cpp_text to a temp file, compile-only (-c), return elapsed seconds or None on error."""
    with tempfile.NamedTemporaryFile(suffix=".cpp", mode="w", delete=False) as f:
        f.write(cpp_text)
        src = f.name

    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as out:
        obj = out.name

    try:
        cmd = (
            [compiler]
            + BASE_FLAGS
            + extra_flags
            + [f"-I{include_dir}"]
            + [f"-D{d}" for d in defines]
            + [src, "-o", obj]
        )
        start = time.perf_counter()
        result = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.perf_counter() - start

        if result.returncode != 0:
            print(f"\n  Compile error:\n{result.stderr[:800]}", file=sys.stderr)
            return None
        return elapsed
    finally:
        os.unlink(src)
        if os.path.exists(obj):
            os.unlink(obj)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base",       type=int,   default=8,
                        help="Base for exponential size sequence (default: 8)")
    parser.add_argument("--steps",      type=int,   default=3,
                        help="Number of steps (default: 3 → sizes BASE^1..BASE^STEPS)")
    parser.add_argument("--compiler",   default="g++",
                        help="C++ compiler to use (default: g++)")
    parser.add_argument("--output",     default="compile_times.json",
                        help="Output JSON file (default: compile_times.json)")
    parser.add_argument("--extra-flags", nargs="*", default=[],
                        help="Extra compiler flags (e.g. for large N: "
                             "--extra-flags -fconstexpr-depth=100000 "
                             "-fconstexpr-loop-limit=100000000 "
                             "-fconstexpr-ops-limit=1000000000)")
    args = parser.parse_args()

    repo_root = Path(__file__).parent.resolve()
    sizes = get_sizes(args.base, args.steps)
    results = []

    print(f"Compiler : {args.compiler}")
    print(f"Sizes    : {sizes}")
    print(f"Strategies: {list(STRATEGIES.keys())}")
    if args.extra_flags:
        print(f"Extra flags: {args.extra_flags}")
    print()

    for n in sizes:
        for strategy_name, strategy_define in STRATEGIES.items():
            cpp_text = (
                f"#define COMPILE_BENCH\n"
                f"#define COMPILE_N {n}\n"
                f"#define {strategy_define}\n"
                f'#include "dispatch_tables.hpp"\n'
                f"int main() {{ return static_cast<int>(bench_size_bytes); }}\n"
            )

            label = f"{strategy_name}/{n}"
            print(f"  {label:<30}", end=" ", flush=True)

            elapsed = compile_and_time(
                cpp_text,
                include_dir=repo_root,
                defines=[],          # defines are embedded in the cpp_text
                extra_flags=args.extra_flags,
                compiler=args.compiler,
            )

            if elapsed is not None:
                print(f"{elapsed:.2f}s")
                results.append({
                    "name":             label,
                    "strategy":         strategy_name,
                    "n":                n,
                    "compile_seconds":  round(elapsed, 4),
                })
            else:
                print("FAILED")

    output_path = Path(args.output)
    with open(output_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults written to {output_path}")


if __name__ == "__main__":
    main()
