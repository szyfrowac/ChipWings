#!/usr/bin/env python3
"""Phase 5 checks for PX4 on U54 hart 1 (run it while sim/phase5/run.sh is up).

    sim/phase5/phase5_test.py                       step 5, PX4 alone      (run.sh headless)
    sim/phase5/phase5_test.py --linux               step 6, PX4 + Linux    (run.sh headless linux)
    sim/phase5/phase5_test.py --target board --serial /dev/ttyUSB1 [--linux --linux-serial /dev/ttyUSB3]

Renode: PX4 NSH on telnet 33457, Linux on 33458, monitor on 33456. Board: MMUART1 / MMUART3 (pyserial).

The pass criteria are the same on both targets; only the Renode run adds the check of mtime against the
simulator's virtual clock, which has no counterpart on the board.

Checks: ver all names the board, SoC ID and MCU; the HRT calibrated the MSS timer; tests hrt passes;
perf latency has most callouts in the 1-5 us buckets; top, work_queue status and uorb top run; a
parameter can be set and reads back as saved. With --linux, also: Linux logs in, hartcheck passes (Linux
on harts 2-4 only), rpmsg_ping round trips to PX4's NuttX, and every PX4 check above still passes with
Linux running. For the long RPMsg run use sim/phase4/rpmsg_test.py 500.

Exit status 0 when every check passes.
"""
import argparse
import random
import re
import socket
import sys
import time

ESC = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


class Console:
    """NSH over a Renode socket terminal or a serial port."""

    def __init__(self, target, port, serial_dev, baud):
        if target == "renode":
            self.s = socket.create_connection(("127.0.0.1", port))
            self.s.settimeout(0.3)
            self._recv = lambda: self.s.recv(65536)
            self._send = self.s.sendall
        else:
            import serial  # pyserial, only needed on the board
            self.s = serial.Serial(serial_dev, baud, timeout=0.3)
            self._recv = lambda: self.s.read(65536)
            self._send = self.s.write

    def _read(self, pattern, timeout):
        """Read until pattern appears. Escape sequences are stripped from the whole buffer, since NSH's
        line editing splits them across reads."""
        raw, end = "", time.time() + timeout
        while time.time() < end:
            try:
                raw += self._recv().decode(errors="replace")
            except socket.timeout:
                pass
            clean = ESC.sub("", raw).replace("\r", "")
            if re.search(pattern, clean):
                return clean
        return ESC.sub("", raw).replace("\r", "")

    def drain(self, quiet=1.0):
        """Discard pending output (e.g. left over from an earlier, interrupted run) until quiet for a while."""
        last = time.time()
        while time.time() - last < quiet:
            try:
                if self._recv():
                    last = time.time()
            except socket.timeout:
                pass

    def start(self):
        """Synchronise: NSH may still be working through input queued by an earlier, interrupted run, so
        wait for a token no earlier run can have produced."""
        self.drain()
        self._send(b"\rset Q 42\r")
        self.seq = random.randrange(10**6)
        self._send(f"echo __SYNC{self.seq} $Q".encode() + b"\r")
        self._read(rf"__SYNC{self.seq} 42\n", 120)

    def run(self, cmd, timeout=60):
        """Run cmd and return its output. The end marker is printed by 'echo __DONE $Q': NSH's echo of the
        command line shows '$Q', only the executed echo shows 42. A prompt is no marker: dmesg replays
        the boot output, prompts included."""
        self.seq += 1
        done = f"__DONE{self.seq}"
        self._send(f"{cmd}; echo {done} $Q".encode() + b"\r")
        out = self._read(rf"{done} 42\n", timeout)
        out = out.rsplit(f"{done} 42", 1)[0]
        # drop NSH's echo of the command line (it redraws the line once per keystroke)
        # (NSH ends that line with a bare CR, which is stripped). The first complete marker is the final
        # redraw; split there, not at the last one: dmesg replays the command line it was started by.
        return out.split(f"{done} $Q", 1)[-1]


class LinuxConsole(Console):
    """BusyBox shell on the Linux console (root/root)."""

    def login(self):
        self.drain()
        self._send(b"\r")
        out = self._read(r"login: $|# $", 30)
        if "login:" in out:
            self._send(b"root\r")
            self._read(r"Password: $", 10)
            self._send(b"root\r")
            self._read(r"# $", 30)

    def run(self, cmd, timeout=120):
        """End marker: the shell's echo shows '$((n+1))', only the executed echo shows the sum. The number is
        new for every command, so output left over from an earlier command cannot end this one."""
        n = random.randrange(10**6, 10**7)
        self._send(f"{cmd}; echo __DONE$(({n}+1))".encode() + b"\r")
        out = self._read(rf"__DONE{n + 1}\n", timeout)
        return out.rsplit(f"__DONE{n + 1}", 1)[0].split(f"__DONE$(({n}+1))", 1)[-1]


class Monitor:
    """Renode monitor, for the virtual-time check."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(0.3)
        time.sleep(0.5)
        self._read(1)

    def _read(self, timeout):
        buf, end = "", time.time() + timeout
        while time.time() < end:
            try:
                buf += self.s.recv(65536).decode(errors="replace")
            except socket.timeout:
                if buf.rstrip().endswith(")"):
                    break
        return ESC.sub("", buf)

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        return self._read(3)


results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", choices=["renode", "board"], default="renode")
    ap.add_argument("--port", type=int, default=33457, help="Renode NSH socket")
    ap.add_argument("--monitor", type=int, default=33456, help="Renode monitor socket")
    ap.add_argument("--serial", default="/dev/ttyUSB1", help="board: MMUART1 serial device")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--linux", action="store_true", help="step 6: Linux runs on harts 2-4 too")
    ap.add_argument("--linux-port", type=int, default=33458, help="Renode Linux console socket")
    ap.add_argument("--linux-serial", default="/dev/ttyUSB3", help="board: MMUART3 serial device")
    args = ap.parse_args()

    if args.linux:
        lx = LinuxConsole(args.target, args.linux_port, args.linux_serial, args.baud)
        lx.login()
        out = lx.run("hartcheck", 60)
        check("linux: hartcheck", "hartcheck: PASS" in out, (re.findall(r"hartcheck: .*", out) or [""])[-1])
        out = lx.run("rpmsg_ping 200 64", 300)
        check("linux: rpmsg_ping to PX4's NuttX", "rpmsg_ping: PASS" in out,
              (re.findall(r"rpmsg_ping: .*", out) or [""])[-1])

    c = Console(args.target, args.port, args.serial, args.baud)
    c.start()

    out = c.run("ver all")
    check("ver all: board", "HW arch: CHIPWINGS_MPFS_HART1" in out)
    check("ver all: SoC ID MPFS (0x000A) in PX4GUID", re.search(r"PX4GUID: 000a", out) is not None)
    check("ver all: MCU", "MCU: MPFS" in out, re.search(r"MCU: .*", out).group(0) if "MCU:" in out else "")
    check("ver all: NuttX", "OS: NuttX" in out)

    out = c.run("dmesg")
    m = re.search(r"INFO  \[arch_hrt\] MSS Timer \d at (\d+) MHz", out)
    check("hrt: MSS timer calibrated against mtime", m is not None and int(m.group(1)) > 1,
          m.group(0) if m else "no [arch_hrt] line in dmesg")

    out = c.run("tests hrt", timeout=120)
    per = [int(x) for x in re.findall(r"\), (\d+)$", out, re.M)]
    check("tests hrt: PASSED", "hrt PASSED" in out and out.count("tick\n") == 3)
    # usleep(100 ms) rounds up to whole 1 ms NuttX ticks plus one, so 100-102 ms
    check("tests hrt: 100 ms sleeps", len(per) == 10 and all(100000 <= p <= 102000 for p in per),
          f"{min(per)}..{max(per)} us" if per else "no samples")

    # Multi-hart Renode delivers interrupts at time-quantum boundaries, and the hart taking the interrupt
    # can be up to a quantum ahead of the source, so there the bound is 5 us plus two quanta (measured:
    # nothing later than 20 us at a 10 us quantum; see px4-linux.resc). On the board it is 5 us.
    limit = 5
    if args.target == "renode":
        q = re.search(r"Quantum: (\d+):(\d+):([\d.]+)", Monitor(args.monitor).cmd("machine GetTimeSourceInfo"))
        quantum_us = round(float(q.group(3)) * 1e6) if q else 100
        if args.linux:
            limit += 2 * quantum_us
    out = c.run("perf latency")
    buckets = [(b, int(n)) for b, n in re.findall(r"^\s*(>?\d+) : (\d+)$", out, re.M)]
    total = sum(n for _, n in buckets)
    fast = sum(n for b, n in buckets if not b.startswith(">") and int(b) <= limit)
    check(f"perf latency: >= 99% of callouts within {limit} us", total > 0 and fast >= 0.99 * total,
          f"{fast}/{total}; " + ", ".join(f"{b}:{n}" for b, n in buckets))

    out = c.run("top once")
    check("top", "Processes:" in out and "Idle_Task" in out)
    out = c.run("work_queue status")
    check("work_queue status", "wq:lp_default" in out)
    out = c.run("uorb top -1")
    check("uorb top", "TOPIC NAME" in out)

    c.run("param set SYS_AUTOSTART 0")
    c.run("param set SYS_AUTOSTART 1100")
    # autosave is deferred, so the saved flag (+) can take a moment to appear
    for _ in range(10):
        out = c.run("param show SYS_AUTOSTART")
        saved = re.search(r"x \+\s+SYS_AUTOSTART .*: 1100", out) is not None
        if saved:
            break
        time.sleep(0.5)
    check("param set/show, saved", saved, re.search(r".*SYS_AUTOSTART .*", out).group(0).strip() if "SYS_AUTOSTART" in out else "")
    c.run("param set SYS_AUTOSTART 0")

    if args.target == "renode":
        mon = Monitor(args.monitor)
        samples = []
        for _ in range(2):
            mon.cmd("pause")
            info = mon.cmd("machine GetTimeSourceInfo")
            mtime = mon.cmd("sysbus ReadQuadWord 0x0200bff8")
            mon.cmd("start")
            v = re.search(r"Elapsed Virtual Time: (\d+):(\d+):([\d.]+)", info)
            t = re.findall(r"0x[0-9A-Fa-f]+", mtime)  # the last one: the first is the echoed address
            if v and len(t) == 2:
                samples.append((int(v.group(1)) * 3600 + int(v.group(2)) * 60 + float(v.group(3)), int(t[-1], 16)))
            time.sleep(3)
        ok = len(samples) == 2 and samples[1][0] > samples[0][0]
        rate = (samples[1][1] - samples[0][1]) / (samples[1][0] - samples[0][0]) if ok else 0
        check("hrt (mtime) advances 1,000,000 per simulated second", ok and abs(rate - 1e6) < 1, f"{rate:.0f}/s")

    print(f"\n{sum(results)}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
