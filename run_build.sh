#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 {pico|pi4} [--no-clean]" >&2
  exit 1
fi

target="$1"
noclean="${2:-}"

"$here/set_build.sh" "$target" >/dev/null
# shellcheck disable=SC1090
source "$here/.build_target"

build_dir="$here/build/${OUT_DIR:-$BUILD_TARGET}"

if [[ "$noclean" != "--no-clean" ]] && [[ -d "$build_dir" ]]; then
  echo "🧹 Cleaning build/${OUT_DIR:-$BUILD_TARGET}"
  rm -rf "$build_dir"
fi

"$here/make_build.sh"

# When building pico firmware, also build the laptop-side host tools
# (pico_provisioner etc.) using the native host compiler in a separate pass.
if [[ "$target" == "pico" ]]; then
  echo ""
  echo "🖥️  Also building host tools (pico_provisioner)..."
  "$here/set_build.sh" host >/dev/null
  host_build_dir="$here/build/host"
  if [[ "$noclean" != "--no-clean" ]] && [[ -d "$host_build_dir" ]]; then
    rm -rf "$host_build_dir"
  fi
  "$here/make_build.sh"
  # Restore pico as the active target so .build_target stays consistent
  "$here/set_build.sh" pico >/dev/null
  echo "✅ Host tools ready: artifacts/host/pico_provisioner"
fi
