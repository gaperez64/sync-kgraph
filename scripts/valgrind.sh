#!/usr/bin/env sh
set -eu

builddir="${1:-build-valgrind}"

if ! command -v valgrind >/dev/null 2>&1; then
  echo "valgrind is required for leak checks" >&2
  exit 127
fi

CC="${CC:-clang}" meson setup "$builddir" --wipe -Dmemgraph=disabled
meson test -C "$builddir" --setup=valgrind --print-errorlogs
