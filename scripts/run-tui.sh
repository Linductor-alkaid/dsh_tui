#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -x "${ROOT}/build/dsh_tui" ]]; then
  cmake -S "${ROOT}" -B "${ROOT}/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${ROOT}/build" -j
fi

"${ROOT}/scripts/setup-profile.sh"

# Important: PATH 里的 `dsh` 可能是 apt dancer's distributed shell。
# 这里显式调用 DeepSeek Harness 的 npx 包。
if [[ -n "${DSH_LAUNCHER:-}" ]]; then
  read -r -a LAUNCHER <<< "${DSH_LAUNCHER}"
else
  LAUNCHER=(npx --yes @deepseek-ai/dsh)
fi
exec "${LAUNCHER[@]}" --profile tui "$@"
