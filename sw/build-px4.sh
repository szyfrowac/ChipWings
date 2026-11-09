#!/usr/bin/env bash
# Build PX4 for the Chipwings flight-control core (PolarFire SoC, U54_1).
#
#   ./build-px4.sh [target]        target = PX4 make target (default: chipwings_mpfs-hart1_default)
#   ./build-px4.sh --toolchain     only (re)create the toolchain shim and test it
#
# Keeps the port outside the PX4 clone: copies sw/px4-chipwings/ over sw/px4/ and applies
# sw/px4-patches/*.patch (once), then runs PX4's make. Output: sw/px4/build/<target>/ and a copy
# of the ELF in sw/out/px4/.
#
# PX4's Toolchain-riscv64-unknown-elf.cmake hard-codes the riscv64-unknown-elf- prefix. We use
# the xPack riscv-none-elf 15.2 toolchain (the one NuttX phases 1-4 were built with), so a shim
# directory provides riscv64-unknown-elf-* wrappers that exec the xPack tools. Wrappers, not
# symlinks: GCC locates cc1, collect2 and the multilib libraries relative to the path it was
# started from.
set -euo pipefail

SW=$(cd "$(dirname "$0")" && pwd)
TARGET=${1:-chipwings_mpfs-hart1_default}
TOOLCHAIN=${TOOLCHAIN:-$HOME/tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin}
SHIM=$SW/out/px4-toolchain
VENV=$SW/px4-venv

[ -x "$TOOLCHAIN/riscv-none-elf-gcc" ] || { echo "no xPack toolchain in $TOOLCHAIN" >&2; exit 1; }
[ -x "$VENV/bin/python" ] || { echo "no venv in $VENV (see sim/README.md, Phase 5)" >&2; exit 1; }

mkdir -p "$SHIM"
for tool in "$TOOLCHAIN"/riscv-none-elf-*; do
  name=riscv64-unknown-elf-${tool##*/riscv-none-elf-}
  printf '#!/bin/sh\nexec "%s" "$@"\n' "$tool" > "$SHIM/$name"
  chmod +x "$SHIM/$name"
done

# Shim and venv first; /usr/bin before the rest so Vivado's bundled ninja is not picked up.
export PATH=$SHIM:$VENV/bin:/usr/bin:$PATH

if [ "$TARGET" = --toolchain ]; then
  t=$(mktemp -d)
  trap 'rm -rf "$t"' EXIT
  printf 'int main(void) { return 0; }\n' > "$t/t.c"
  riscv64-unknown-elf-gcc -march=rv64gc -mabi=lp64d -mcmodel=medany --specs=nosys.specs \
    -o "$t/t.elf" "$t/t.c" 2>/dev/null  # nosys.specs warns about stub syscalls; expected
  riscv64-unknown-elf-readelf -h "$t/t.elf" | grep -E 'Class|Machine|Flags'
  echo "cmake $(cmake --version | head -1 | cut -d' ' -f3), ninja $(ninja --version), $(python --version)"
  python -c 'import em, kconfiglib, jinja2, yaml, pymavlink, nunavut' && echo "venv: PX4 Python modules import"
  exit 0
fi

if [ -d "$SW/px4-chipwings" ]; then
  cp -r "$SW/px4-chipwings/." "$SW/px4/"
fi

# NuttX's own build does not depend on its config: after a defconfig change only files whose source
# changed are rebuilt, so objects built under the old config (struct layouts depend on CONFIG_*) are
# linked against code built under the new one and crash in confusing ways (a NULL function pointer in
# uart_open after enabling serial DMA). Delete every NuttX object when the board's defconfig changes.
NUTTX_TREE=$SW/px4/platforms/nuttx/NuttX
DEFCONFIG_STAMP=$SW/out/patch-stamps/nuttx-defconfig.sha
new_sum=$(cat "$SW"/px4/boards/*/*/nuttx-config/*/defconfig 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat "$DEFCONFIG_STAMP" 2>/dev/null)" != "$new_sum" ]; then
  echo "NuttX defconfig changed: removing stale NuttX objects"
  find "$NUTTX_TREE/nuttx" "$NUTTX_TREE/apps" -name '*.o' -delete 2>/dev/null || true
  mkdir -p "$(dirname "$DEFCONFIG_STAMP")"
  echo "$new_sum" > "$DEFCONFIG_STAMP"
fi

# Applied patches are recorded in out/patch-stamps/<repo>: a later patch in a series can change the
# context of an earlier one, which then neither applies nor reverse-applies on its own.
apply_once() {  # repo patch
  local stamp
  stamp=$SW/out/patch-stamps/$(basename "$1")
  mkdir -p "$(dirname "$stamp")"
  if git -C "$1" apply --check "$2" 2>/dev/null; then
    echo "applying $(basename "$2")"
    git -C "$1" apply "$2"
    echo "$(basename "$2")" >> "$stamp"
  elif grep -qxF "$(basename "$2")" "$stamp" 2>/dev/null; then
    :
  elif git -C "$1" apply --check --reverse "$2" 2>/dev/null; then
    echo "$(basename "$2")" >> "$stamp"
  else
    echo "patch neither applies nor is already applied: $2" >&2
    exit 1
  fi
}

for p in "$SW"/px4-patches/*.patch; do
  [ -e "$p" ] || continue
  apply_once "$SW/px4" "$p"
done

# Our NuttX fixes (sw/patches, written against NuttX 13.1) that PX4's NuttX fork (12.12) also needs.
# Not 0001: the double mtimecmp offset it fixes came in after 12.12 (12.12's riscv_mtimer adds no
# per-hart offset, so the fork's mpfs_timerisr.c is right as it is; applying 0001 there silences
# hart 1's tick). 0003 (IHC) is rebased for the fork in px4-patches/nuttx/ (only context lines differ).
for p in "$SW"/patches/0002-*.patch "$SW"/px4-patches/nuttx/*.patch; do
  apply_once "$SW/px4/platforms/nuttx/NuttX/nuttx" "$p"
done

make -C "$SW/px4" "$TARGET"

ELF=$SW/px4/build/$TARGET/$TARGET.elf
mkdir -p "$SW/out/px4"
cp "$ELF" "$SW/out/px4/"
echo
echo "Built $TARGET: $SW/out/px4/$TARGET.elf"
riscv64-unknown-elf-size "$ELF"
