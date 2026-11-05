#!/usr/bin/env bash
# Ask the running Renode (monitor on localhost:33456) what each PolarFire SoC core is doing.
# Proves from outside the guest that only U54_1 (hart 1) executes code.
#
#   ./check-one-core.sh [port]
set -euo pipefail
python3 -I - "${1:-33456}" <<'PY'
import re, socket, sys, time

s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.settimeout(1.0)

def ask(cmd):
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.4)
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    text = buf.decode(errors="replace").replace("\r", "")
    lines = [l.strip() for l in text.split("\n") if l.strip() and not l.strip().startswith("(") and l.strip() != cmd]
    return lines[-1] if lines else "?"

ask("")
ask("mach set 0")
cores = [("e51", 0, "bootloader (HSS)"), ("u54_1", 1, "NuttX"), ("u54_2", 2, "Linux"),
         ("u54_3", 3, "Linux"), ("u54_4", 4, "Linux")]
before = {c: int(ask(f"{c} ExecutedInstructions"), 0) for c, _, _ in cores}
time.sleep(2)
print(f"{'core':6} {'hart':>4}  {'planned role':17} {'halted':6} {'instructions executed':>22} {'in last 2 s':>12}  PC")
for c, hart, role in cores:
    total = int(ask(f"{c} ExecutedInstructions"), 0)
    print(f"{c:6} {hart:>4}  {role:17} {ask(f'{c} IsHalted'):6} {total:>22,} {total - before[c]:>12,}  {ask(f'{c} PC')}")
PY
