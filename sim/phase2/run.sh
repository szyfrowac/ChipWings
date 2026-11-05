#!/usr/bin/env bash
# Start Linux on U54 harts 2-4 in Renode (Phase 2).
#
#   ./run.sh          GUI: Renode monitor window + a terminal window on MMUART3 (Linux console)
#   ./run.sh headless no windows; console on telnet localhost 33458, monitor on telnet localhost 33456
#
# Either way the console is also logged to logs/linux-mmuart3.log.
# MONITOR_PORT / LINUX_PORT (default 33456 / 33458) let it run alongside another instance.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RENODE=${RENODE:-$HOME/tools/renode_1.17.0-portable/renode}
MONITOR_PORT=${MONITOR_PORT:-33456}
LINUX_PORT=${LINUX_PORT:-33458}
mkdir -p "$HERE/logs"
rm -f "$HERE/logs/linux-mmuart3.log"

case "${1:-gui}" in
  gui)
    exec "$RENODE" -P "$MONITOR_PORT" -e "\$linuxport=$LINUX_PORT; i @$HERE/linux-harts234.resc; showAnalyzer mmuart3; start" ;;
  headless)
    exec "$RENODE" --disable-gui -p -P "$MONITOR_PORT" --pid-file "$HERE/logs/renode.pid" \
      -e "\$linuxport=$LINUX_PORT; i @$HERE/linux-harts234.resc; start" ;;
  *)
    echo "usage: $0 [gui|headless]" >&2; exit 1 ;;
esac
