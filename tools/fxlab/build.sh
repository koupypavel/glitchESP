#!/usr/bin/env bash
# Build fxlab with gcc or clang on Linux/macOS: tools/fxlab/out/fxlab
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
: "${CC:=cc}"
"$CC" -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -I../../firmware/main/effects \
    fxlab.c ../../firmware/main/effects/*.c -lm -o out/fxlab
echo "built $PWD/out/fxlab"
