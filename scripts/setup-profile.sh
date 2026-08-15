#!/usr/bin/env bash
set -euo pipefail

# Create (or repair) the DeepSeek Harness `tui` profile and point it at the
# in-repo `packages/dsh-tui` bundle. We never invoke a bare `dsh` here: apt's
# dancer's-shell also ships /usr/bin/dsh, so launch through npx explicitly.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSH_HOME="${DSH_HOME:-${HOME}/.dsh}"
PROFILE_DIR="${DSH_HOME}/profiles/tui"
MODULES_DIR="${PROFILE_DIR}/node_modules"

mkdir -p "${PROFILE_DIR}" "${MODULES_DIR}"

if [[ ! -f "${PROFILE_DIR}/package.json" ]]; then
  cat > "${PROFILE_DIR}/package.json" <<EOF
{
  "name": "dsh-profile-tui",
  "private": true,
  "dependencies": {
    "dsh-tui": "file:${ROOT}/packages/dsh-tui"
  },
  "dsh": {
    "profile": {
      "bundles": [
        "@deepseek-ai/dsh-base",
        "dsh-tui"
      ]
    }
  }
}
EOF
fi

if [[ ! -f "${PROFILE_DIR}/cordis.patch.yml" ]]; then
  cat > "${PROFILE_DIR}/cordis.patch.yml" <<'EOF'
# Your patch layer for the dsh TUI profile. Add overrides here.
[]
EOF
fi

if [[ ! -f "${PROFILE_DIR}/pnpm-workspace.yaml" ]]; then
  cat > "${PROFILE_DIR}/pnpm-workspace.yaml" <<'EOF'
packages:
  - .

nodeLinker: hoisted
autoInstallPeers: false
EOF
fi

LINK="${MODULES_DIR}/dsh-tui"
TARGET="${ROOT}/packages/dsh-tui"
if [[ ! -e "${LINK}" ]] || [[ -L "${LINK}" && "$(readlink -f "${LINK}")" != "$(readlink -f "${TARGET}")" ]]; then
  rm -f "${LINK}"
  ln -s "${TARGET}" "${LINK}"
fi

echo "DeepSeek Harness TUI profile ready: ${PROFILE_DIR}"
echo "Run: npx @deepseek-ai/dsh --profile tui"
