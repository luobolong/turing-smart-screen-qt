#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/linux --parallel 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux --output-on-failure
