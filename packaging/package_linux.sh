#!/usr/bin/env bash
# Build the Linux alpha operator bundle and separate developer-tools bundle.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$REPO_ROOT/build-release"
OUTPUT_DIR="$REPO_ROOT/dist/linux"

echo "=== Packaging ProjectUltra operator bundle for Linux ==="

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DULTRA_BUILD_TESTS=OFF \
  -DULTRA_BUILD_GUI=ON

cmake --build "$BUILD_DIR" --parallel "$(nproc)" --target \
  ultra ultra_tnc ultra_gui \
  cli_simulator threaded_simulator test_waveform_simple decode_bench session_decode

"$SCRIPT_DIR/package_operator_bundle.sh" "$BUILD_DIR" linux "dist/linux"

(
  cd "$OUTPUT_DIR"
  rm -f projectultra-linux.zip dev-tools-linux.zip
  zip -r projectultra-linux.zip projectultra-linux
  zip -r dev-tools-linux.zip dev-tools-linux
)

echo ""
echo "=== Linux Bundles Complete ==="
echo "Operator bundle: $OUTPUT_DIR/projectultra-linux.zip"
echo "Developer tools: $OUTPUT_DIR/dev-tools-linux.zip"
