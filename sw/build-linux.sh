#!/usr/bin/env bash
# Build the Linux side of the Chipwings system (U54 harts 2-4) with Buildroot.
#
#   ./build-linux.sh            configure (first time) and build
#   ./build-linux.sh <target>   any Buildroot target, e.g. linux-rebuild, menuconfig, savedefconfig
#
# Sources: sw/buildroot (2026.02.3 LTS), our external tree sw/br2-chipwings, output in sw/br2-out.
# Images:  sw/br2-out/images/{fw_jump.elf, Image, chipwings-mpfs-linux.dtb}
# A full build takes roughly 20-40 min; after editing the device tree run `./build-linux.sh linux-rebuild all`.
set -euo pipefail

SW=$(cd "$(dirname "$0")" && pwd)
BR="make -C $SW/buildroot O=$SW/br2-out BR2_EXTERNAL=$SW/br2-chipwings"

[ -f "$SW/br2-out/.config" ] || $BR chipwings_mpfs_linux_defconfig
if [ $# -eq 0 ]; then
  $BR
else
  $BR "$@"
fi
ls -la "$SW/br2-out/images"
