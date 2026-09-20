#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --verbose
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --parallel
ctest --preset asan-ubsan --verbose
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --verbose
build/release/atlas-chaos --iterations "${1:-1000}" --seed 20260920
for run in 1 2 3; do build/release/atlas-bench --records 10000; done
build/release/atlas-example build/release/example.db
build/release/atlasdb verify build/release/example.db
