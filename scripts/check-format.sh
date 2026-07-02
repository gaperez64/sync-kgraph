#!/usr/bin/env sh
set -eu

clang-format --dry-run --Werror \
  include/sync_kgraph/sync.h \
  src/sync.c \
  src/sync_cli.c \
  src/memgraph/sync_module.c \
  tests/test_core.c
