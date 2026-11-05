#!/usr/bin/env bash
# Start the AMP system with RPMsg in Renode (Phase 4): NuttX on U54 hart 1, Linux on U54 harts 2-4.
#
#   ./run.sh          GUI: Renode monitor window + terminal windows on MMUART1 (NSH) and MMUART3 (Linux)
#   ./run.sh headless no windows; NSH on telnet localhost 33457, Linux on 33458, monitor on 33456
#
# Either way both consoles are also logged to logs/.
# MONITOR_PORT / NSH_PORT / LINUX_PORT let it run alongside another instance.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RENODE=${RENODE:-$HOME/tools/renode_1.17.0-portable/renode}
MONITOR_PORT=${MONITOR_PORT:-33456}
NSH_PORT=${NSH_PORT:-33457}
LINUX_PORT=${LINUX_PORT:-33458}
mkdir -p "$HERE/logs"
rm -f "$HERE/logs/nuttx-mmuart1.log" "$HERE/logs/linux-mmuart3.log"
VARS="\$nshport=$NSH_PORT; \$linuxport=$LINUX_PORT"

case "${1:-gui}" in
  gui)
    exec "$RENODE" -P "$MONITOR_PORT" -e "$VARS; i @$HERE/rpmsg.resc; showAnalyzer mmuart1; showAnalyzer mmuart3; start" ;;
  headless)
    exec "$RENODE" --disable-gui -p -P "$MONITOR_PORT" --pid-file "$HERE/logs/renode.pid" \
      -e "$VARS; i @$HERE/rpmsg.resc; start" ;;
  *)
    echo "usage: $0 [gui|headless]" >&2; exit 1 ;;
esac
