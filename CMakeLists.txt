cmake_minimum_required(VERSION 3.14)
project(MessageDispatchBenchmark VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# We need large constexpr evaluation depth for the compile-time operations (especially quicksort and perfect hash).
# Add it specifically for GCC/Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fconstexpr-depth=100000 -fconstexpr-loop-limit=100000000 -fconstexpr-ops-limit=1000000000)
endif()

# FetchContent to download Google Benchmark
include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable benchmark testing" FORCE)

FetchContent_Declare(
  benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz
)
FetchContent_MakeAvailable(benchmark)

# Add our benchmark executable
add_executable(dispatch_benchmark benchmark.cpp)

# Link against Google Benchmark
target_link_libraries(dispatch_benchmark PRIVATE benchmark::benchmark benchmark::benchmark_main)

# Add optimization flags since we're benchmarking
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dispatch_benchmark PRIVATE -O3 -march=native)
endif()
