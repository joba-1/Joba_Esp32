#!/usr/bin/env bash
set -euo pipefail

echo "Generating Doxygen documentation..."

if [ ! -f Doxyfile ]; then
  echo "Doxyfile not found in repo root"
  exit 1
fi

# Run doxygen using the repo Doxyfile
doxygen Doxyfile

if [ -d docs/doxygen/html ]; then
  echo "Doxygen HTML generated at docs/doxygen/html"
  exit 0
else
  echo "Doxygen did not produce html output"
  exit 2
fi
