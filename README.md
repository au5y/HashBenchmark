# Static Message Dispatch Benchmark

A C++17 benchmark comparing three **zero-allocation, compile-time dispatch strategies** for mapping message IDs to callbacks — targeting embedded systems (Zephyr RTOS, ARM Cortex-M) where dynamic memory allocation is prohibited.

All dispatch tables are built as `constexpr` and placed in `.rodata`/flash, consuming **zero RAM** at runtime.

## Strategies

| Strategy | Lookup | Build complexity | Best for |
|---|---|---|---|
| **PerfectHash** | O(1) — 2 hash evals + 2 reads | O(N²) constexpr | N > 64, sparse/arbitrary IDs |
| **SortedArray** | O(log N) — binary search | O(N log N) constexpr | N < 64, simplest to audit |
| **StructuredTrie** | O(1) — 2 array reads + bit-shifts | O(N) constexpr | IDs with bit-field structure |

The 16-bit message ID layout used by the benchmark: `[15:11]` = subsystem (5 bits), `[10:0]` = message type (11 bits).

## Benchmark metrics

1. **Runtime access** — CPU time per 10,000 lookups (mix of hits and misses)
2. **Container size** — bytes of `.rodata` occupied by each dispatch table
3. **Compile time** — constexpr construction cost paid at build time, per strategy per N

## Prerequisites

- CMake ≥ 3.14
- Conan 2.x (`pip install conan`)
- GCC or Clang with C++17
- Python 3.10+ with `matplotlib` (`pip install -r requirements.txt`)

## Quick start

```bash
pip install -r requirements.txt
./build.sh
```

Outputs: `benchmark_results.json`, `compile_times.json`, `benchmark_plot.png`.

## Configuration

Benchmark sizes are `BASE^1, BASE^2, ..., BASE^STEPS`. Defaults: `BASE=8, STEPS=3` → `{8, 64, 512}`.

```bash
# Custom sizes — steps=4 gives {8, 64, 512, 4096}
./build.sh --base 8 --steps 4

# Skip the per-strategy compile-time measurement
./build.sh --skip-compile-bench

# Specify a different C++ compiler for compile-time measurement
./build.sh --compiler clang++
```

CMake directly:

```bash
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBENCH_BASE=8 \
    -DBENCH_STEPS=3
cmake --build build -j$(nproc)
```

> **Large N (STEPS ≥ 4):** The PerfectHash table uses O(N²) constexpr construction. For N ≥ 4096, add elevated constexpr limits:
> ```
> -DCMAKE_CXX_FLAGS="-fconstexpr-depth=100000 -fconstexpr-loop-limit=100000000 -fconstexpr-ops-limit=1000000000"
> ```

## Running benchmarks manually

```bash
# All strategies
./build/dispatch_benchmark

# Filter to a single strategy
./build/dispatch_benchmark --benchmark_filter=PerfectHash

# JSON output for plotting
./build/dispatch_benchmark --benchmark_format=json --benchmark_out=benchmark_results.json

# Compile-time measurement only
python3 measure_compile_time.py --base 8 --steps 3

# Plot from existing JSON files
python3 plot_benchmarks.py --runtime benchmark_results.json \
                            --compile compile_times.json \
                            --output  benchmark_plot.png
```

## Project structure

```
benchmark.cpp          — Google Benchmark harness; size sequence via BENCH_BASE/BENCH_STEPS
dispatch_tables.hpp    — All three dispatch table implementations (constexpr)
measure_compile_time.py— Per-(strategy,N) compile-time measurement script
plot_benchmarks.py     — 3-panel plot: runtime, container size, compile time
build.sh               — Full build + benchmark + plot pipeline
CMakeLists.txt         — Build config; exposes BENCH_BASE and BENCH_STEPS
conanfile.txt          — Google Benchmark 1.8.3 via Conan
Approaches.md          — Detailed algorithmic notes and embedded-system tradeoffs
```

## Key design notes

- **Zero RAM**: all tables are `static constexpr`, landing in `.rodata`/flash on ARM.
- **StructuredTrie size is constant** regardless of N — it always allocates a full `32 × 2048` pointer array (~512 KiB on 64-bit). This is intentional and visible in the size panel of the plot.
- **Test data**: 10,000 random 16-bit IDs (seed 42). Most lookups are misses, which exercises all code paths including the "not found" return.
- See `Approaches.md` for production usage guidance, MISRA notes, and Zephyr integration details.
