#!/usr/bin/env bash
# OpenCrawlerCpp 一键构建（POSIX / CI）
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo "构建完成：build/opencrawler-cpp"
echo "自测：./build/opencrawler-cpp selftest"
