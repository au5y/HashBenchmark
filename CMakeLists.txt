cmake_minimum_required(VERSION 3.14)
project(MessageDispatchBenchmark VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# ---------------------------------------------------------------------------
# Benchmark size configuration
# Sizes run: BENCH_BASE^1, BENCH_BASE^2, ..., BENCH_BASE^BENCH_STEPS
# Default: 8^1=8, 8^2=64, 8^3=512
#
# For larger N (BENCH_STEPS >= 4), you may need elevated constexpr limits.
# Add to your cmake invocation:
#   -DCMAKE_CXX_FLAGS="-fconstexpr-depth=100000 -fconstexpr-loop-limit=100000000 -fconstexpr-ops-limit=1000000000"
# ---------------------------------------------------------------------------
set(BENCH_BASE  "8" CACHE STRING "Base for exponential benchmark size sequence")
set(BENCH_STEPS "3" CACHE STRING "Number of exponential steps (sizes = BASE^1 .. BASE^STEPS)")

# Use Conan for Google Benchmark
find_package(benchmark REQUIRED)

add_executable(dispatch_benchmark benchmark.cpp)

target_compile_definitions(dispatch_benchmark PRIVATE
    BENCH_BASE=${BENCH_BASE}
    BENCH_STEPS=${BENCH_STEPS}
)

target_link_libraries(dispatch_benchmark PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dispatch_benchmark PRIVATE -O3 -march=native)
endif()
