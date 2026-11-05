#!/usr/bin/env bash
# Start PX4 on U54_1 (Linux off) in Renode (Phase 5).
#
#   ./run.sh          GUI: Renode monitor window + a terminal window on MMUART1 (the NSH console)
#   ./run.sh headless no windows; NSH on telnet localhost 33457, monitor on telnet localhost 33456
#   ./run.sh <mode> linux  also boot Linux on harts 2-4 (px4-linux.resc); its console on 33458
#   ./run.sh stop     quit a running instance (through the monitor; SIGTERM alone can leave it up)
#
# Either way the console is also logged to logs/px4-mmuart1.log.
# MONITOR_PORT / NSH_PORT (default 33456 / 33457) let a second instance run alongside the first.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RENODE=${RENODE:-$HOME/tools/renode_1.17.0-portable/renode}
MONITOR_PORT=${MONITOR_PORT:-33456}
NSH_PORT=${NSH_PORT:-33457}
stop() {
  local pid
  pid=$(cat "$HERE/logs/renode.pid" 2>/dev/null) || return 0
  kill -0 "$pid" 2>/dev/null || return 0
  { printf 'quit\n'; sleep 1; } | timeout 5 nc -q1 localhost "$MONITOR_PORT" >/dev/null 2>&1 || true
  for _ in $(seq 20); do kill -0 "$pid" 2>/dev/null || return 0; sleep 0.5; done
  kill -9 "$pid" 2>/dev/null || true
}

if [ "${1:-}" = stop ]; then stop; exit 0; fi
stop  # never start a second instance on the same ports

mkdir -p "$HERE/logs"
rm -f "$HERE/logs/px4-mmuart1.log"

RESC=px4.resc
EXTRA=
if [ "${2:-}" = linux ]; then
  RESC=px4-linux.resc
  EXTRA="\$linuxport=${LINUX_PORT:-33458}; "
  rm -f "$HERE/logs/linux-mmuart3.log"
fi

case "${1:-gui}" in
  gui)
    exec "$RENODE" -P "$MONITOR_PORT" -e "\$nshport=$NSH_PORT; $EXTRA""i @$HERE/$RESC; showAnalyzer mmuart1; start" ;;
  headless)
    exec "$RENODE" --disable-gui -p -P "$MONITOR_PORT" --pid-file "$HERE/logs/renode.pid" \
      -e "\$nshport=$NSH_PORT; $EXTRA""i @$HERE/$RESC; start" ;;
  *)
    echo "usage: $0 [gui|headless]" >&2; exit 1 ;;
esac
