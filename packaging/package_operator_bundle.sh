#!/usr/bin/env bash
# Create the alpha operator bundle and the separate developer-tools bundle.

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 <build-dir> <target> [output-dir]" >&2
  echo "  target: linux | macos | windows | local" >&2
  exit 2
fi

BUILD_DIR=$1
TARGET=$2
OUTPUT_DIR=${3:-package}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR_ABS=$(cd -- "$BUILD_DIR" && pwd)
if [[ "$OUTPUT_DIR" = /* ]]; then
  OUTPUT_DIR_ABS="$OUTPUT_DIR"
else
  OUTPUT_DIR_ABS="$REPO_ROOT/$OUTPUT_DIR"
fi

operator_root="$OUTPUT_DIR_ABS/projectultra-$TARGET"
dev_root="$OUTPUT_DIR_ABS/dev-tools-$TARGET"

rm -rf "$operator_root" "$dev_root"
mkdir -p "$operator_root/tools" "$operator_root/docs" "$dev_root"

copy_binary() {
  local name=$1
  local dest=$2
  local found=0
  local candidates=(
    "$BUILD_DIR_ABS/$name"
    "$BUILD_DIR_ABS/Release/$name"
    "$BUILD_DIR_ABS/$name.exe"
    "$BUILD_DIR_ABS/Release/$name.exe"
  )

  for path in "${candidates[@]}"; do
    if [[ -f "$path" ]]; then
      cp "$path" "$dest/"
      found=1
    fi
  done

  [[ "$found" -eq 1 ]]
}

require_binary() {
  local name=$1
  local dest=$2
  if ! copy_binary "$name" "$dest"; then
    echo "Required binary not found in $BUILD_DIR_ABS: $name" >&2
    exit 1
  fi
}

optional_binary() {
  local name=$1
  local dest=$2
  copy_binary "$name" "$dest" || true
}

require_binary ultra "$operator_root"
require_binary ultra_tnc "$operator_root"
require_binary ultra_gui "$operator_root"

optional_binary cli_simulator "$dev_root"
optional_binary threaded_simulator "$dev_root"
optional_binary test_waveform_simple "$dev_root"
optional_binary decode_bench "$dev_root"
optional_binary session_decode "$dev_root"

if ! compgen -G "$dev_root/*" >/dev/null; then
  echo "No developer tools found to package." >&2
  exit 1
fi

cp "$REPO_ROOT/tools/ultra_tnc.conf.example" "$operator_root/tools/"
cp "$REPO_ROOT/README.md" "$operator_root/"
cp "$REPO_ROOT/docs/TNC_INTERFACE.md" "$operator_root/docs/"
cp "$REPO_ROOT/docs/README.md" "$operator_root/docs/"

if [[ -f "$REPO_ROOT/docs/RUNNING.md" ]]; then
  cp "$REPO_ROOT/docs/RUNNING.md" "$operator_root/RUNNING.md"
fi

if [[ "$TARGET" == "windows" ]]; then
  sdl_found=0
  declare -a dll_candidates=()

  if [[ -n "${VCPKG_ROOT:-}" ]]; then
    vcpkg_unix="$VCPKG_ROOT"
    if command -v cygpath >/dev/null 2>&1; then
      vcpkg_unix=$(cygpath -u "$VCPKG_ROOT")
    fi
    dll_candidates+=("${vcpkg_unix}/installed/x64-windows/bin/SDL2.dll")
    dll_candidates+=("${vcpkg_unix}/installed/x64-windows/bin/SDL2d.dll")
  fi

  dll_candidates+=("/c/vcpkg/installed/x64-windows/bin/SDL2.dll")
  dll_candidates+=("/c/vcpkg/installed/x64-windows/bin/SDL2d.dll")
  dll_candidates+=("C:/SDL2/lib/x64/SDL2.dll")

  for dll in "${dll_candidates[@]}"; do
    if [[ -f "$dll" ]]; then
      cp "$dll" "$operator_root/"
      cp "$dll" "$dev_root/"
      sdl_found=1
    fi
  done

  if [[ "$sdl_found" -eq 0 ]]; then
    echo "SDL2 runtime DLL was not found. Expected from vcpkg or C:/SDL2." >&2
    exit 1
  fi
fi

cat > "$operator_root/BUNDLE.txt" <<EOF
ProjectUltra alpha operator bundle ($TARGET)

Included operator programs:
- ultra_tnc
- ultra_gui
- ultra

Operator docs:
- RUNNING.md (added when docs/RUNNING.md exists in the checkout)
- README.md
- docs/TNC_INTERFACE.md
- docs/README.md
- tools/ultra_tnc.conf.example

Developer simulators and bench tools are packaged separately in dev-tools-$TARGET.
EOF

cat > "$dev_root/BUNDLE.txt" <<EOF
ProjectUltra developer tools ($TARGET)

This artifact is for simulation, bench, and decode diagnostics. It is not the
default operator download.
EOF
