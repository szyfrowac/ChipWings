#!/usr/bin/env python3
"""RPMsg round-trip test for the Phase 4 simulation (run it while sim/phase4/run.sh is up).

Logs into Linux on its console (telnet port 33458), binds rpmsg_char to NuttX's
"rpmsg-ping" channel (/dev/rpmsg0) and sends N ping requests. NuttX's rpmsg_ping
endpoint on hart 1 echoes each one back; the payload of every reply is checked.

    sim/phase4/rpmsg_test.py [rounds] [--linux-port 33458]
    sim/phase4/rpmsg_test.py [rounds] --target board [--linux-serial /dev/ttyUSB3]

Exit status 0 when every round trip came back intact.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
from cwconsole import Console as _Console, add_target_args  # noqa: E402

# Runs inside Linux (BusyBox sh). Request = rpmsg_ping_msg_s: cmd (ACK bit) | len | cookie | data.
GUEST_SCRIPT = r"""
cat > /tmp/pingtest.sh <<'EOF'
#!/bin/sh
N=${1:-100}; ok=0; bad=0; t0=$(cut -d' ' -f1 /proc/uptime)
exec 3<>/dev/rpmsg0
i=1
while [ $i -le $N ]; do
  p=$(printf 'msg%05d-chipwings' $i)
  printf '\001\000\000\000\042\000\000\000\000\000\000\000\000\000\000\000%s' "$p" >&3
  r=$(dd bs=64 count=1 <&3 2>/dev/null | tail -c 18)
  if [ "$r" = "$p" ]; then ok=$((ok+1)); else bad=$((bad+1)); echo "mismatch at $i: '$r'"; fi
  i=$((i+1))
done
exec 3>&-
t1=$(cut -d' ' -f1 /proc/uptime)
echo "RESULT rounds=$N ok=$ok bad=$bad guest_seconds=$t0..$t1"
EOF
chmod +x /tmp/pingtest.sh
"""

BIND = ("modprobe rpmsg_char; D=$(ls /sys/bus/rpmsg/devices | grep rpmsg-ping); "
        "[ -e /dev/rpmsg0 ] || { echo rpmsg_chrdev > /sys/bus/rpmsg/devices/$D/driver_override; "
        "echo $D > /sys/bus/rpmsg/drivers/rpmsg_chrdev/bind; }; ls /dev/rpmsg0")


class Console(_Console):
    def run(self, cmd, timeout):
        """Run a shell command, return its output (up to an end marker that only the command's own echo produces)."""
        self.send(cmd + "; echo __END__$((40+2))")
        out, _ = self.read_until(r"__END__42", timeout)
        return out.split("__END__$((40+2))")[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rounds", nargs="?", type=int, default=100)
    add_target_args(ap)
    args = ap.parse_args()

    c = Console.open(args, "linux")
    c.login()

    for line in GUEST_SCRIPT.strip().split("\n"):
        c.send(line)
    c.run("true", 10)

    out = c.run(BIND, 15)
    if "/dev/rpmsg0" not in out:
        print("could not bind rpmsg_char to the rpmsg-ping channel:\n" + out)
        return 1

    out = c.run(f"/tmp/pingtest.sh {args.rounds}", 30 + args.rounds)
    for line in out.split("\n"):
        if line.startswith("mismatch") or line.startswith("RESULT"):
            print(line)
    m = re.search(r"RESULT rounds=(\d+) ok=(\d+) bad=(\d+)", out)
    if not m:
        print(f"no result: RPMsg stalled (timeout). Last output:\n{out[-400:]}")
        return 1
    return 0 if m.group(2) == m.group(1) else 1


if __name__ == "__main__":
    sys.exit(main())
