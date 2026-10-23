#!/usr/bin/env bash
set -euo pipefail
r3_source="$(cd "$(dirname "$0")" && pwd)"
r3_root="$(git -C "$r3_source" rev-parse --show-toplevel)"
cd "$r3_root"
probe_dir="$PWD/Main/sim/build/r3-review-20261023"
mkdir -p "$probe_dir"
if [ "$r3_source" != "$probe_dir" ]; then
  cp "$r3_source/setup_probe.c" "$probe_dir/setup_probe.c"
fi
probe_gcc="$(find "$HOME/opt" -path '*/bin/riscv-none-elf-gcc' -print -quit)"
test -n "$probe_gcc"
"$probe_gcc" --version > "$probe_dir/probe-compiler.log"
make -C Main/sw/apps/test/rastTest -j8 PLATFORM=target \
  RISCV_BIN="${probe_gcc%gcc}" APP=r3SetupProbe \
  APP_SRCDIR="$probe_dir" APP_SRCS=setup_probe.c \
  OBJDIR="$probe_dir/target" > "$probe_dir/probe-build.log" 2>&1
