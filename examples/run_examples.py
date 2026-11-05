#!/usr/bin/env python3
"""Run every Chipwings example in the Phase 4 simulation and report PASS/FAIL.

Start the simulation first (sim/phase4/run.sh headless), then:

    examples/run_examples.py [--nsh-port 33457] [--linux-port 33458]

NuttX side (NSH on MMUART1): cw_timer, cw_threads, then starts the RPMsg servers
cw_rpmsg_echo and cw_rpmsg_imu. Linux side (MMUART3, root/root): hartcheck,
rpmsg_ping, rpmsg_echo, rpmsg_imu. Exit status 0 when everything passed.
"""
import argparse
import re
import socket
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


class Console:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(0.3)

    def read_until(self, pattern, timeout):
        buf, end = "", time.time() + timeout
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if d:
                    buf += ANSI.sub("", d.decode(errors="replace")).replace("\r", "")
                    m = re.search(pattern, buf)
                    if m:
                        return buf, m
            except socket.timeout:
                pass
        return buf, None

    def send(self, line):
        self.s.sendall(line.encode() + b"\r")


def run(console, name, command, timeout):
    """Run one example; it prints '<name>: PASS' or '<name>: FAIL' last."""
    console.send(command)
    out, m = console.read_until(rf"{name}: (PASS|FAIL)", timeout)
    for line in out.split("\n"):
        line = line.strip()
        if line.startswith(f"{name}:") and line != f"{name}: PASS":
            print(f"    {line}")
    result = m.group(1) if m else "TIMEOUT"
    print(f"  {name:14} {result}")
    return result == "PASS"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--nsh-port", type=int, default=33457)
    ap.add_argument("--linux-port", type=int, default=33458)
    args = ap.parse_args()
    results = []

    print("NuttX (hart 1):")
    nsh = Console(args.nsh_port)
    nsh.send("")
    nsh.read_until(r"nsh> ", 5)
    results.append(run(nsh, "cw_timer", "cw_timer 100 10", 60))
    results.append(run(nsh, "cw_threads", "cw_threads 1000", 60))

    # RPMsg servers for the Linux side; both print their endpoint once it exists
    nsh.send("cw_rpmsg_echo")
    nsh.read_until(r"cw_rpmsg_echo: (endpoint|running)", 10)
    nsh.send("cw_rpmsg_imu &")
    nsh.read_until(r"cw_rpmsg_imu: endpoint|cw_rpmsg_imu: running", 10)
    print("  cw_rpmsg_echo, cw_rpmsg_imu started")

    print("Linux (harts 2-4):")
    lx = Console(args.linux_port)
    lx.send("")
    out, _ = lx.read_until(r"login:|# $", 10)
    if "login:" in out:
        lx.send("root")
        lx.read_until(r"Password:", 5)
        lx.send("root")
        lx.read_until(r"# $", 10)
    time.sleep(1)  # name-service announcements of the new NuttX endpoints
    results.append(run(lx, "hartcheck", "hartcheck", 30))
    results.append(run(lx, "rpmsg_ping", "rpmsg_ping 200 64", 120))
    results.append(run(lx, "rpmsg_echo", "rpmsg_echo 496", 120))
    results.append(run(lx, "rpmsg_imu", "rpmsg_imu 100 500", 120))

    nsh.send("cw_rpmsg_echo")
    out, m = nsh.read_until(r"cw_rpmsg_echo: running[^\n]*\n", 5)
    if m:
        print("NuttX side after the run: " + m.group(0).strip())

    passed = sum(results)
    print(f"\n{passed}/{len(results)} examples passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
