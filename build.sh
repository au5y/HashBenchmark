#!/bin/bash
set -e

rm -rf build
mkdir -p build

conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release

cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
./dispatch_benchmark
