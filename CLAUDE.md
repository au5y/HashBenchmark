# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

**Full pipeline (build + runtime benchmark + compile-time measurement + plot):**
```bash
./build.sh                          # defaults: base=8, steps=3 → sizes {8, 64, 512}
./build.sh --base 8 --steps 4      # larger sizes {8, 64, 512, 4096} — slower compile
./build.sh --skip-compile-bench    # skip per-(strategy,N) compile-time measurement
./build.sh --compiler clang++      # use clang++ for compile-time measurement
```

**Manual build:**
```bash
pip install -r requirements.txt    # matplotlib
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBENCH_BASE=8 -DBENCH_STEPS=3
cmake --build build -j$(nproc)
```

**Run benchmarks individually:**
```bash
./build/dispatch_benchmark                                   # all
./build/dispatch_benchmark --benchmark_filter=PerfectHash   # single strategy
./build/dispatch_benchmark --benchmark_format=json --benchmark_out=benchmark_results.json
```

**Compile-time measurement only:**
```bash
python3 measure_compile_time.py --base 8 --steps 3
```

**Plot only (from existing JSON):**
```bash
python3 plot_benchmarks.py --runtime benchmark_results.json \
                            --compile compile_times.json \
                            --output  benchmark_plot.png
```

## Architecture

### File layout
- `dispatch_tables.hpp` — all three dispatch strategy implementations (`constexpr`)
- `benchmark.cpp` — Google Benchmark harness; size sequence config; `state.counters["bytes"]` for container sizes
- `measure_compile_time.py` — compiles isolated (strategy, N) programs and times each
- `plot_benchmarks.py` — 3-panel plot: runtime, container size, compile time
- `build.sh` — full pipeline with `--base`/`--steps`/`--compiler` args
- `CMakeLists.txt` — exposes `BENCH_BASE`/`BENCH_STEPS` as CMake cache variables
- `Approaches.md` — detailed algorithmic notes and embedded-system tradeoffs

### Three dispatch strategies (all in `dispatch_tables.hpp`)

1. **PerfectHash** — CHD/Hanov minimal perfect hash; `G[]` displacement table built at compile time. O(1) lookup: two `mph_hash` calls + two array reads. O(N²) constexpr build (selection sort dominates). Size: `N * (4 + 4 + 8)` bytes.

2. **SortedArray** — `{msg_id, callback}` pairs sorted at compile time via `cx_quicksort`. O(log N) binary search at runtime. O(N log N) constexpr build. Simplest to audit. Size: `N * ~16` bytes.

3. **StructuredTrie** — two-level array indexed by bit-fields: `[15:11]` = subsystem (5 bits), `[10:0]` = message type (11 bits). O(1): two array reads + bit-shifts. Size: always `32 * 2048 * sizeof(MsgCallback)` ≈ 512 KiB regardless of N.

### Size sequence configuration

Benchmark sizes are generated as `BASE^1, BASE^2, ..., BASE^STEPS` via template metaprogramming (`Pow<B,E>` + `ExpSeq<Base,Steps>` in `benchmark.cpp`). Controlled by `BENCH_BASE`/`BENCH_STEPS` macros — set via CMake `-DBENCH_BASE=N -DBENCH_STEPS=N` or directly with `-D` compiler flags.

Defaults: `BASE=8, STEPS=3` → `{8, 64, 512}`. These compile in 2–3 minutes without elevated constexpr flags. For `STEPS=4` (max N=4096), add:
```
-DCMAKE_CXX_FLAGS="-fconstexpr-depth=100000 -fconstexpr-loop-limit=100000000 -fconstexpr-ops-limit=1000000000"
```

### `Tables<N>` template

`Tables<N>` in `dispatch_tables.hpp` holds all three tables as `static constexpr` members (`PH`, `SA`, `TRIE`). Instantiating `Tables<N>` for each benchmark size forces constexpr evaluation of all three strategies simultaneously. The `COMPILE_BENCH` section in `dispatch_tables.hpp` is used by `measure_compile_time.py` to instantiate strategies individually for isolated timing.

### `COMPILE_BENCH` mode

`measure_compile_time.py` generates a `.cpp` per (strategy, N):
```cpp
#define COMPILE_BENCH
#define COMPILE_N 64
#define STRATEGY_PH        // or STRATEGY_SA / STRATEGY_TRIE
#include "dispatch_tables.hpp"
int main() { return static_cast<int>(bench_size_bytes); }
```
`dispatch_tables.hpp` under `COMPILE_BENCH` builds just the selected strategy and exposes `bench_size_bytes` as a `constexpr`. The compiler must fully evaluate the constexpr to compile the program.

### Benchmark JSON fields
- `cpu_time` — nanoseconds per 10k-lookup iteration
- `bytes` — dispatch table size in bytes (from `state.counters["bytes"]`)
