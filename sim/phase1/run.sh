#!/usr/bin/env bash
# Start NuttX on U54_1 in Renode (Phase 1).
#
#   ./run.sh          GUI: Renode monitor window + a terminal window on MMUART1 (the NSH console)
#   ./run.sh headless no windows; NSH on telnet localhost 33457, monitor on telnet localhost 33456
#
# Either way the console is also logged to logs/nuttx-mmuart1.log.
# MONITOR_PORT / NSH_PORT (default 33456 / 33457) let a second instance run alongside the first.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RENODE=${RENODE:-$HOME/tools/renode_1.17.0-portable/renode}
MONITOR_PORT=${MONITOR_PORT:-33456}
NSH_PORT=${NSH_PORT:-33457}
mkdir -p "$HERE/logs"
rm -f "$HERE/logs/nuttx-mmuart1.log"

case "${1:-gui}" in
  gui)
    exec "$RENODE" -P "$MONITOR_PORT" -e "\$nshport=$NSH_PORT; i @$HERE/nuttx-hart1.resc; showAnalyzer mmuart1; start" ;;
  headless)
    exec "$RENODE" --disable-gui -p -P "$MONITOR_PORT" --pid-file "$HERE/logs/renode.pid" \
      -e "\$nshport=$NSH_PORT; i @$HERE/nuttx-hart1.resc; start" ;;
  *)
    echo "usage: $0 [gui|headless]" >&2; exit 1 ;;
esac
