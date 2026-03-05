#!/bin/bash
set -e

rm -rf build
mkdir -p build

conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release

cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

echo "Running benchmarks..."
./dispatch_benchmark --benchmark_format=json --benchmark_out=benchmark_results.json

echo "Generating plot..."
python3 ../plot_benchmarks.py benchmark_results.json