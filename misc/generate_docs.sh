#!/usr/bin/env bash
# Generate Doxygen documentation (docs/doxygen)
# - ensures output dir exists
# - runs doxygen with the repository Doxyfile
# - prints helpful hints if `dot` or `doxygen` are missing

set -euo pipefail
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
DOXYFILE="$ROOT_DIR/Doxyfile"
OUTDIR="$ROOT_DIR/docs/doxygen"

command -v doxygen >/dev/null 2>&1 || { echo "doxygen not found; install doxygen (e.g. apt install doxygen graphviz)" >&2; exit 2; }
command -v dot >/dev/null 2>&1 || echo "warning: dot not found; diagrams will be disabled in output"

mkdir -p "$OUTDIR"
cd "$ROOT_DIR"

echo "Running doxygen..."
doxygen "$DOXYFILE"

echo "Done. HTML docs: $OUTDIR/html/index.html"

echo "If you want PDF via LaTeX, run:"
echo "  cd $OUTDIR/latex && make"
