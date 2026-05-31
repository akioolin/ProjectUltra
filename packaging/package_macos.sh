#!/usr/bin/env bash
# Build the macOS alpha operator bundle and separate developer-tools bundle.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$REPO_ROOT/build-release"
OUTPUT_DIR="$REPO_ROOT/dist/macos"

echo "=== Packaging ProjectUltra operator bundle for macOS ==="

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DULTRA_BUILD_TESTS=OFF \
  -DULTRA_BUILD_GUI=ON

cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)" --target \
  ultra ultra_tnc ultra_gui \
  ota_simulator measure_ack_fer

"$SCRIPT_DIR/package_operator_bundle.sh" "$BUILD_DIR" macos "dist/macos"

(
  cd "$OUTPUT_DIR"
  rm -f projectultra-macos.zip dev-tools-macos.zip
  zip -r projectultra-macos.zip projectultra-macos
  zip -r dev-tools-macos.zip dev-tools-macos
)

echo ""
echo "=== macOS Bundles Complete ==="
echo "Operator bundle: $OUTPUT_DIR/projectultra-macos.zip"
echo "Developer tools: $OUTPUT_DIR/dev-tools-macos.zip"
