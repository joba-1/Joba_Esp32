#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/coverage/run_coverage.sh [env]
# Default env: native (PlatformIO test environment)

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

ENV="${1:-native}"
OUTDIR="coverage"
INFO="$OUTDIR/coverage.info"
FILTERED="$OUTDIR/coverage.info.filtered"

echo "Coverage run: env=$ENV -> output=$OUTDIR"

command -v lcov >/dev/null 2>&1 || { echo "lcov not found; install lcov/genhtml" >&2; exit 2; }
command -v genhtml >/dev/null 2>&1 || { echo "genhtml not found; install lcov/genhtml" >&2; exit 2; }

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

echo "Zeroing existing counters in build dir..."
# Determine PlatformIO build dir for the environment
PIO_BUILD_DIR=".pio/build/$ENV"
if [ -d "$PIO_BUILD_DIR" ]; then
  lcov --directory "$PIO_BUILD_DIR" --zerocounters || true
else
  lcov --directory . --zerocounters || true
fi

echo "Running unit tests (PlatformIO)..."
platformio test -e "$ENV"

echo "Capturing coverage data from build dir..."
# Capture coverage from PlatformIO build dir where .gcda files are produced
if [ -d "$PIO_BUILD_DIR" ]; then
  lcov --capture --directory "$PIO_BUILD_DIR" --output-file "$INFO" || {
    echo "lcov capture from $PIO_BUILD_DIR failed; trying project root..." >&2
    lcov --capture --directory . --output-file "$INFO"
  }
else
  lcov --capture --directory . --output-file "$INFO"
fi

echo "Filtering out system and test framework files..."
lcov --ignore-errors unused --remove "$INFO" '/usr/*' '*/.pio/*' '*/test/*' --output-file "$FILTERED"

echo "Generating HTML report..."
genhtml --demangle-cpp --legend --output-directory "$OUTDIR/html" "$FILTERED"

echo "Coverage HTML available at: $OUTDIR/html/index.html"
echo "Also produced lcov info: $FILTERED"

# Produce Cobertura XML via gcovr if available
if command -v gcovr >/dev/null 2>&1; then
  echo "Generating Cobertura XML via gcovr..."
  gcovr -r . --xml -o "$OUTDIR/cobertura-coverage.xml"
fi

echo "Done."
