#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -x "${ROOT}/build/dsh_tui" ]]; then
  cmake -S "${ROOT}" -B "${ROOT}/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${ROOT}/build" -j
fi

# dsh_tui 会自行初始化 DeepSeek Harness 的 tui profile 并拉起桥接进程。
# 可用环境变量：
#   DSH_HOME  DeepSeek Harness home（默认 ~/.dsh）
#   DSH_BIN   DeepSeek Harness dsh 可执行文件（默认通过 npx 调用）
exec "${ROOT}/build/dsh_tui" "$@"
