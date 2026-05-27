#!/usr/bin/env bash
set -Eeuo pipefail

find_picotool_pkgdir() {
  local p="$1"  # tool_prefix
  local cand=(
    "$p/lib/cmake/picotool"
    "$p/lib64/cmake/picotool"
    "$p/share/picotool/cmake"
    "$p/lib/picotool/cmake"
  )
  for d in "${cand[@]}"; do
    [[ -f "$d/picotoolConfigVersion.cmake" ]] && { echo "$d"; return 0; }
  done
  return 1
}


# Debug: RUN_VERBOSE=1 ./run_build pico
[[ "${RUN_VERBOSE:-0}" == "1" ]] && set -x
trap 'echo -e "\n❌ Failed at line $LINENO: $BASH_COMMAND"; exit 1' ERR

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

dir_populated() {
  local d="$1"
  [[ -d "$d" ]] || return 1
  shopt -s nullglob dotglob
  local items=("$d"/*)
  shopt -u nullglob dotglob
  (( ${#items[@]} > 0 ))
}

if [[ ! -f "$here/.build_target" ]]; then
  echo "No build target selected. Run: ./set_build pico | pi4"
  exit 1
fi
# shellcheck disable=SC1090
source "$here/.build_target"

[[ -z "${BUILD_TARGET:-}" ]] && { echo "Malformed .build_target (missing BUILD_TARGET)."; exit 2; }

build_dir="$here/build/${OUT_DIR:-$BUILD_TARGET}"
echo "🏗️  Building: $BUILD_TARGET  →  build/${OUT_DIR:-$BUILD_TARGET}"

mkdir -p "$build_dir"

# --- auto-init required submodules (once) ---
repo_root="$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$repo_root" ]]; then
  echo "❌ Git metadata not found. Please clone (not a ZIP):"
  echo "   git clone --recurse-submodules <repo_url>"
  exit 1
fi

# Always need pico-sdk and mbedtls; picotool only for pico builds
req=( "$here/lib/pico-sdk" "$here/lib/mbedtls" )
[[ "$BUILD_TARGET" == "pico" ]] && req+=( "$here/lib/picotool" )

need_init=0
for d in "${req[@]}"; do
  if ! dir_populated "$d"; then need_init=1; break; fi
done

if (( need_init )); then
  echo "🔄 Initializing submodules..."
  git -C "$repo_root" submodule sync --recursive
  if [[ -n "${SUBMODULE_DEPTH:-}" ]]; then
    git -C "$repo_root" submodule update --init --recursive --depth "$SUBMODULE_DEPTH"
  else
    git -C "$repo_root" submodule update --init --recursive
  fi
  echo "✅ Submodules ready."
fi
# === ensure picotool (Pico only; silent + reproducible) ===
if [[ "$BUILD_TARGET" == "pico" ]]; then
  tool_prefix="$here/.tooling/picotool"

  # submodule must be populated (auto-init above should have done this)
  if ! dir_populated "$here/lib/picotool"; then
    echo "❌ embedded/lib/picotool is empty. Run: git submodule update --init --recursive"
    exit 1
  fi

  # Try reuse of an existing local install
  pkgdir="$(find_picotool_pkgdir "$tool_prefix" || true)"
  need_build=1
  [[ -n "$pkgdir" ]] && need_build=0

  if (( need_build )); then
    if ! pkg-config --exists libusb-1.0 2>/dev/null; then
      echo "❌ Missing deps: libusb-1.0-0-dev pkg-config (Ubuntu/WSL)"
      echo "   sudo apt install -y libusb-1.0-0-dev pkg-config"
      exit 1
    fi

    echo "🔧 Building local picotool (once) ..."
    : "${PICO_SDK_PATH:=$here/lib/pico-sdk}"   # default to our submodule
    if [[ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]]; then
      echo "Error: pico-sdk submodule missing at: $PICO_SDK_PATH"
      echo "Run: git submodule update --init --recursive"
      exit 1
    fi
    export PICO_SDK_PATH
    cmake -S "$here/lib/picotool" -B "$here/build/_picotool" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="$tool_prefix" \
          -DCMAKE_INSTALL_LIBDIR=lib \
          -DPICO_SDK_PATH="$PICO_SDK_PATH"
    cmake --build "$here/build/_picotool" -j
    cmake --install "$here/build/_picotool"

    # Resolve where it actually installed
    pkgdir="$(find_picotool_pkgdir "$tool_prefix")" || {
      echo "❌ picotool installed, but package files not found under $tool_prefix"; exit 1; }
  fi

  # Tell CMake to use our installed package (silences SDK warning)
  export picotool_DIR="$pkgdir"

  # Print a clean, single-line version
  ver="$(awk -F'"' '/PACKAGE_VERSION/ {print $2; exit}' "$pkgdir/picotoolConfigVersion.cmake" 2>/dev/null || true)"
  if [[ -n "$ver" ]]; then
    echo "🔎 picotool: using v${ver} at $pkgdir"
  else
    echo "🔎 picotool: using package at $pkgdir"
  fi
fi

# CONFIGURE BUILD (after picotool export)
# Set PICO_SDK_PATH for Pico builds (always use local submodule)
if [[ "$BUILD_TARGET" == "pico" ]]; then
  PICO_SDK_PATH="$here/lib/pico-sdk"
  export PICO_SDK_PATH
  echo "🔧 Using PICO_SDK_PATH: $PICO_SDK_PATH"
fi

cmake -S "$here" -B "$build_dir"
jobs=4; command -v nproc >/dev/null 2>&1 && jobs="$(nproc)"
cmake --build "$build_dir" -j"$jobs"

echo "✅ Build complete: $BUILD_TARGET (artifacts in build/${OUT_DIR:-$BUILD_TARGET})"

# === Collect artifacts (history + latest file + checksum + manifest) ===
art_dir="$here/artifacts/$BUILD_TARGET"
mkdir -p "$art_dir"

ts="$(date +%Y%m%d-%H%M%S)"
repo_dir="$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || echo "$here")"
git_desc="$(git -C "$repo_dir" describe --always --dirty --tags 2>/dev/null \
        || git -C "$repo_dir" rev-parse --short HEAD 2>/dev/null \
        || date +%Y%m%d)"
ver_tag="${git_desc:-unknown}"

# === Collect artifacts (history + latest file + checksum + manifest) ===
# Map legacy 'pi4' to 'receiver' if needed, otherwise use BUILD_TARGET
if [[ "$BUILD_TARGET" == "pi4" ]]; then
  art_dir_name="receiver"
else
  art_dir_name="$BUILD_TARGET"
fi
art_dir="$here/artifacts/$art_dir_name"
mkdir -p "$art_dir"

ts="$(date +%m%d-%H_%M)"
repo_dir="$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || echo "$here")"
git_desc="$(git -C "$repo_dir" describe --always --tags 2>/dev/null \
        || git -C "$repo_dir" rev-parse --short HEAD 2>/dev/null \
        || date +%Y%m%d)"
echo "Version: $git_desc"
ver_tag="${git_desc:-unknown}"

write_manifest() {
  local path="$1" target="$2" ver="$3" stamp="$4" file="$5"
  printf '{ "target":"%s", "version":"%s", "timestamp":"%s", "file":"%s" }\n' \
    "$target" "$ver" "$stamp" "$file" > "$path"
}

if [[ "$BUILD_TARGET" == "pico" ]]; then
  # 1. LiFi session sender (SST auth + provisioner support) — primary firmware
  uf2="$build_dir/sender/lifi_session_sender.uf2"
  if [[ -f "$uf2" ]]; then
    fname="${ts}_lifi_session_sender.uf2"
    cp -f -- "$uf2" "$art_dir/$fname"
    (cd "$art_dir" && sha256sum "$fname" > "$fname.sha256" && ln -sf "$fname" "lifi_session_sender.uf2")
    manifest="$art_dir/${ts}_lifi_session_sender.json"
    write_manifest "$manifest" "pico" "$ver_tag" "$ts" "$fname"
    echo "📦 UF2 (session/SST): $art_dir/$fname"
  else
    echo "⚠️ lifi_session_sender.uf2 not found"
  fi

  # 2. Speed test sender
  uf2="$build_dir/sender/pico_speed_test_sender.uf2"
  if [[ -f "$uf2" ]]; then
    fname="${ts}_pico_speed_test_sender.uf2"
    cp -f -- "$uf2" "$art_dir/$fname"
    (cd "$art_dir" && sha256sum "$fname" > "$fname.sha256")
    manifest="$art_dir/${ts}_pico_speed_test_sender.json"
    write_manifest "$manifest" "pico" "$ver_tag" "$ts" "$fname"
    echo "📦 UF2 (speed test): $art_dir/$fname"
  fi

elif [[ "$BUILD_TARGET" == "pi4" || "$BUILD_TARGET" == "receiver" ]]; then
  # 1. Flash Receiver (Original Session Receiver)
  # Look for 'flash_receiver' (was lifi_session_receiver)
  # Attempt to find the new name first, fallback if not found? 
  # Actually, we rely on cmake having built the new name.
  
  exe="$(find "$build_dir/receiver" -maxdepth 1 -type f -name 'flash_receiver' -executable -print -quit)"
  if [[ -n "$exe" ]]; then
    # User requested name: flash_receiver
    # We prepend timestamp but keep the base name
    fname="${ts}_flash_receiver"
    install -m 0755 -- "$exe" "$art_dir/$fname"
    (cd "$art_dir" && sha256sum "$fname" > "$fname.sha256" && ln -sf "$fname" "flash_receiver")
    manifest="$art_dir/${ts}_flash_receiver.json"
    write_manifest "$manifest" "pi4_flash" "$ver_tag" "$ts" "$fname"
    echo "📦 EXE: $art_dir/$fname"
    echo "🔗 Link: $art_dir/flash_receiver"
  else
    # Fallback to old name if build wasn't clean, but warn
    echo "⚠️ flash_receiver executable not found"
  fi

  # 2. Key Manager (Sender Logic)
  exe="$(find "$build_dir/receiver" -maxdepth 1 -type f -name 'keys_receiver' -executable -print -quit)"
  if [[ -n "$exe" ]]; then
    fname="${ts}_keys_receiver"
    install -m 0755 -- "$exe" "$art_dir/$fname"
    (cd "$art_dir" && sha256sum "$fname" > "$fname.sha256" && ln -sf "$fname" "keys_receiver")
    manifest="$art_dir/${ts}_keys_receiver.json"
    write_manifest "$manifest" "pi4_keys" "$ver_tag" "$ts" "$fname"
    echo "📦 EXE: $art_dir/$fname"
    echo "🔗 Link: $art_dir/keys_receiver"
  else
     echo "⚠️ keys_receiver executable not found"
  fi

  # 3. Asker (Listener Logic)
  exe="$(find "$build_dir/receiver" -maxdepth 1 -type f -name 'ask_receiver' -executable -print -quit)"
  if [[ -n "$exe" ]]; then
    fname="${ts}_ask_receiver"
    install -m 0755 -- "$exe" "$art_dir/$fname"
    (cd "$art_dir" && sha256sum "$fname" > "$fname.sha256" && ln -sf "$fname" "ask_receiver")
    manifest="$art_dir/${ts}_ask_receiver.json"
    write_manifest "$manifest" "pi4_ask" "$ver_tag" "$ts" "$fname"
    echo "📦 EXE: $art_dir/$fname"
    echo "🔗 Link: $art_dir/ask_receiver"
  else
    echo "⚠️ ask_receiver executable not found"
  fi

elif [[ "$BUILD_TARGET" == "host" ]]; then
  exe="$(find "$build_dir" -type f -name 'pico_provisioner' -executable -print -quit)"
  if [[ -z "$exe" ]]; then
    echo "⚠️ pico_provisioner executable not found under $build_dir"
  else
    fname="${ts}_pico_provisioner"
    install -m 0755 -- "$exe" "$art_dir/$fname"
    (cd "$art_dir" && sha256sum "$fname" > "$fname.sha256" && ln -sf "$fname" "pico_provisioner")
    manifest="$art_dir/${ts}_pico_provisioner.json"
    write_manifest "$manifest" "host" "$ver_tag" "$ts" "$fname"
    echo "📦 EXE (host): $art_dir/$fname"
    echo "🔗 Link: $art_dir/pico_provisioner"
  fi
fi




# === Prune: keep only the last M complete builds (artifact + .sha256 + .json) ===
M_BUILDS="${KEEP_BUILDS:-3}"   # override with: KEEP_BUILDS=N ./run_build <target>

# newest→oldest manifests (exclude 'latest.json'); NUL-safe
mapfile -d '' manifests < <(
  find "$art_dir" -maxdepth 1 -type f -name '*.json' ! -name 'latest.json' \
       -printf '%T@ %p\0' \
  | sort -z -nr -k1,1 \
  | cut -z -d' ' -f2-
)

echo "🔎 Prune sees ${#manifests[@]} build(s); keeping $M_BUILDS"
if (( ${#manifests[@]} > M_BUILDS )); then
  echo "🧹 Pruning old builds…"
  for (( i=M_BUILDS; i<${#manifests[@]}; i++ )); do
    m="${manifests[$i]}"

    # Extract the JSON "file" value: look for the token file, then the next quoted string
    base="$(awk -F'"' '{for (i=1;i<NF;i++) if ($i=="file") {print $(i+2); exit}}' "$m" 2>/dev/null || true)"

    if [[ -n "$base" ]]; then
      echo "   - removing $(basename "$base") (+ .sha256, manifest)"
      rm -f -- "$art_dir/$base" "$art_dir/$base.sha256" "$m"
    else
      # Fallback: derive from manifest stem (works with our naming)
      stem="${m%.json}"
      echo "   - removing $(basename "$stem") (+ .sha256, manifest)"
      rm -f -- "$stem" "$stem.sha256" "$m"
    fi
  done
fi

echo "🗂  Collected: $art_dir (keeping last ${M_BUILDS} builds)"
