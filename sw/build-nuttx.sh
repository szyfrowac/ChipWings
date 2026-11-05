#!/usr/bin/env bash
# Build NuttX for the Chipwings flight-control core (PolarFire SoC, U54_1).
#
#   ./build-nuttx.sh [config]      config = a directory under sw/configs (default: nsh-hart1)
#
# Applies sw/patches to sw/nuttx (once), links sw/chipwings-apps into sw/apps as "chipwings" and
# examples/nuttx as "cwexamples",
# configures the in-tree icicle:nsh board, replaces .config with sw/configs/<config>/defconfig,
# and builds. If the config directory has an ld.script, it replaces the board's linker script
# (nsh-amp-hart1 uses this to link at 0xA2000000).
# Output: sw/nuttx/nuttx (ELF) and sw/nuttx/nuttx.bin, also copied to sw/out/<config>/.
set -euo pipefail

SW=$(cd "$(dirname "$0")" && pwd)
CONFIG=${1:-nsh-hart1}
TOOLCHAIN=${TOOLCHAIN:-$HOME/tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin}
export PATH=$TOOLCHAIN:$PATH

[ -f "$SW/configs/$CONFIG/defconfig" ] || { echo "no such config: $SW/configs/$CONFIG/defconfig" >&2; exit 1; }

# Applied patches are recorded in out/patch-stamps/nuttx-13.1: a later patch can change the context
# of an earlier one, which then neither applies nor reverse-applies on its own (as in build-px4.sh).
STAMP=$SW/out/patch-stamps/nuttx-13.1
mkdir -p "$(dirname "$STAMP")"
for p in "$SW"/patches/*.patch; do
  if git -C "$SW/nuttx" apply --check "$p" 2>/dev/null; then
    echo "applying $(basename "$p")"
    git -C "$SW/nuttx" apply "$p"
    echo "$(basename "$p")" >> "$STAMP"
  elif grep -qxF "$(basename "$p")" "$STAMP" 2>/dev/null; then
    :
  elif git -C "$SW/nuttx" apply --check --reverse "$p" 2>/dev/null; then
    echo "$(basename "$p")" >> "$STAMP"
  else
    echo "patch neither applies nor is already applied: $p" >&2
    exit 1
  fi
done

ln -sfn ../chipwings-apps "$SW/apps/chipwings"
# Example/test apps (cw_timer, cw_threads, cw_rpmsg_echo, cw_rpmsg_imu): examples/nuttx
ln -sfn ../../examples/nuttx "$SW/apps/cwexamples"

cd "$SW/nuttx"
[ -f .config ] && make distclean > /dev/null
./tools/configure.sh -l icicle:nsh > /dev/null
cp "$SW/configs/$CONFIG/defconfig" .config
make olddefconfig > /dev/null
LDARGS=()
if [ -f "$SW/configs/$CONFIG/ld.script" ]; then
  # Make.defs builds ARCHSCRIPT from the board's scripts; a command-line value overrides it.
  # Copied into the board directory so the preprocessed .tmp stays in the NuttX tree.
  cp "$SW/configs/$CONFIG/ld.script" boards/risc-v/mpfs/icicle/scripts/ld-chipwings.script
  LDARGS=(ARCHSCRIPT="$SW/nuttx/boards/risc-v/mpfs/icicle/scripts/ld-chipwings.script")
fi
make -j"$(nproc)" "${LDARGS[@]}"

mkdir -p "$SW/out/$CONFIG"
cp nuttx nuttx.bin System.map "$SW/out/$CONFIG/"

echo
echo "Built $CONFIG: $SW/out/$CONFIG/nuttx"
riscv-none-elf-size nuttx
