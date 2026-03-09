#!/bin/bash
# Build, benchmark, measure compile time, and plot results.
#
# Usage:
#   ./build.sh [--base N] [--steps N] [--compiler CXX] [--skip-compile-bench]
#
# Options:
#   --base   N   Base for exponential size sequence (default: 8)
#   --steps  N   Number of exponential steps (default: 3 → {8, 64, 512})
#                Use --steps 4 for {8,64,512,4096} — requires elevated constexpr flags
#   --compiler   C++ compiler for compile-time measurement (default: g++)
#   --skip-compile-bench  Skip the per-(strategy,N) compile time measurement
#
# Examples:
#   ./build.sh                        # default: base=8, steps=3
#   ./build.sh --base 8 --steps 4    # larger sizes; slower compile
#   ./build.sh --skip-compile-bench  # runtime + sizes only

set -e

BENCH_BASE=8
BENCH_STEPS=3
COMPILER=g++
SKIP_COMPILE_BENCH=0

while [[ $# -gt 0 ]]; do
    case $1 in
        --base)               BENCH_BASE=$2;        shift 2 ;;
        --steps)              BENCH_STEPS=$2;       shift 2 ;;
        --compiler)           COMPILER=$2;          shift 2 ;;
        --skip-compile-bench) SKIP_COMPILE_BENCH=1; shift   ;;
        *)  echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

echo "=== Configuration ==="
echo "  BENCH_BASE  = ${BENCH_BASE}"
echo "  BENCH_STEPS = ${BENCH_STEPS}"
echo "  Sizes       = $(python3 -c "b,s=${BENCH_BASE},${BENCH_STEPS}; print([b**i for i in range(1,s+1)])")"
echo "  Compiler    = ${COMPILER}"
echo ""

# ---------------------------------------------------------------------------
# 1. Build the runtime benchmark binary via Conan + CMake
# ---------------------------------------------------------------------------
echo "=== Building runtime benchmark ==="
rm -rf build
mkdir -p build

conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release

cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBENCH_BASE="${BENCH_BASE}" \
    -DBENCH_STEPS="${BENCH_STEPS}"

BUILD_START=$(date +%s%N)
cmake --build build -j"$(nproc)"
BUILD_END=$(date +%s%N)
BUILD_SECS=$(( (BUILD_END - BUILD_START) / 1000000000 ))
echo "  Full build time: ${BUILD_SECS}s"
echo ""

# ---------------------------------------------------------------------------
# 2. Run runtime benchmarks
# ---------------------------------------------------------------------------
echo "=== Running runtime benchmarks ==="
./build/dispatch_benchmark \
    --benchmark_format=json \
    --benchmark_out=benchmark_results.json
echo ""

# ---------------------------------------------------------------------------
# 3. Measure per-(strategy, N) compile time
# ---------------------------------------------------------------------------
if [[ $SKIP_COMPILE_BENCH -eq 0 ]]; then
    echo "=== Measuring compile times ==="
    python3 measure_compile_time.py \
        --base    "${BENCH_BASE}" \
        --steps   "${BENCH_STEPS}" \
        --compiler "${COMPILER}" \
        --output  compile_times.json
    echo ""
fi

# ---------------------------------------------------------------------------
# 4. Plot results
# ---------------------------------------------------------------------------
echo "=== Generating plot ==="
PLOT_ARGS="--runtime benchmark_results.json"
if [[ $SKIP_COMPILE_BENCH -eq 0 && -f compile_times.json ]]; then
    PLOT_ARGS="${PLOT_ARGS} --compile compile_times.json"
fi
# shellcheck disable=SC2086
python3 plot_benchmarks.py ${PLOT_ARGS} --output benchmark_plot.png --report results.md

echo ""
echo "Done. Outputs:"
echo "  benchmark_results.json  — raw Google Benchmark data"
[[ $SKIP_COMPILE_BENCH -eq 0 ]] && echo "  compile_times.json      — per-(strategy,N) compile times"
echo "  benchmark_plot.png      — comparison plot"
echo "  results.md              — markdown summary with tables and plot"
