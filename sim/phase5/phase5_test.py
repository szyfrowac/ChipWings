#!/usr/bin/env python3
"""Phase 5 checks for PX4 on U54 hart 1 (run it while sim/phase5/run.sh is up).

    sim/phase5/phase5_test.py                       step 5, PX4 alone      (run.sh headless)
    sim/phase5/phase5_test.py --linux               step 6, PX4 + Linux    (run.sh headless linux)
    sim/phase5/phase5_test.py --linux --flight      steps 7-8, flight with Linux (run.sh headless linux)
    sim/phase5/phase5_test.py --target board --serial /dev/ttyUSB1 [--linux --linux-serial /dev/ttyUSB3 [--flight]]

Renode: PX4 NSH on telnet 33457, Linux on 33458, monitor on 33456. Board: MMUART1 / MMUART3 (pyserial).

The pass criteria are the same on both targets; only the Renode run adds the check of mtime against the
simulator's virtual clock, which has no counterpart on the board.

Checks: ver all names the board, SoC ID and MCU; the HRT calibrated the MSS timer; tests hrt passes;
perf latency has most callouts in the 1-5 us buckets; top, work_queue status and uorb top run; a
parameter can be set and reads back as saved. With --linux, also: Linux logs in, hartcheck passes (Linux
on harts 2-4 only), rpmsg_ping round trips to PX4's NuttX, and every PX4 check above still passes with
Linux running. For the long RPMsg run use sim/phase4/rpmsg_test.py 500.

--flight (needs --linux; steps 7 and 8, about 10 minutes of simulated time) flies the SIH quad with Linux
as the companion computer: 60 s of MAVLink over /dev/ttyRPMSG0 (heartbeat 1 Hz, ATTITUDE 25 Hz, no CRC
errors or sequence gaps); arm and takeoff commanded from Linux; EKF2 healthy and the hover holding;
stress-ng on the Linux harts while hovering (hover holds, no failsafe, no late HRT callouts); and finally
Linux is lost (Renode: harts 2-4 halted from the monitor; board: sysrq panic) and PX4 must declare the
data link lost, land and disarm on its own. Run it once per boot, on a freshly booted system: the dmesg
check needs the boot lines, and the flight leaves the vehicle disarmed on the ground with Linux gone.
sim/phase5/isolation_test.py is the Renode variant with a kernel panic instead (it checks only that PX4
keeps flying and the shell comes back: a panicked Linux slows Renode to about 0.03x real time).

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

    def close(self):
        # Renode's telnet monitor answers only one connection at a time: a connection left open starves
        # every later one (commands are accepted and silently ignored).
        self.s.close()


results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))


def vehicle(c):
    """(z in m, PX4 clock in s) from vehicle_local_position."""
    out = c.run("listener vehicle_local_position -n 1")
    z = re.search(r"^\s+z: (-?[\d.]+)", out, re.M)
    t = re.search(r"^\s+timestamp: (\d+)", out, re.M)
    return (float(z.group(1)) if z else None), (int(t.group(1)) / 1e6 if t else None)


def commander(c):
    """(navigation mode, in failsafe, armed)."""
    out = c.run("commander status")
    mode = re.search(r"navigation mode: (\w+)", out)
    return (mode.group(1) if mode else "?"), "in failsafe: yes" in out, "Armed" in out and "Disarmed" not in out


def late_callouts(c):
    """HRT callouts later than 100 us since the last `perf reset`."""
    out = c.run("perf latency")
    return sum(int(n) for b, n in re.findall(r"^\s*(>?\d+) : (\d+)$", out, re.M) if b.startswith(">") or int(b) >= 1000)


def hovering(z):
    return z is not None and abs(z + 2.5) < 0.2


def freeze_linux(args, lx):
    """Make Linux disappear; returns (how, confirmed). Renode: halt harts 2-4 (a hung Linux, and the
    simulation keeps its speed), confirmed by reading IsHalted back. Board: panic it (nothing to read back)."""
    if args.target == "renode":
        mon = Monitor(args.monitor)
        try:
            for h in (2, 3, 4):
                mon.cmd(f"sysbus.u54_{h} IsHalted true")
            halted = [re.search(r"\bTrue\b", mon.cmd(f"sysbus.u54_{h} IsHalted")) is not None for h in (2, 3, 4)]
        finally:
            mon.close()
        return "harts 2-4 halted", all(halted)
    lx._send(b"echo c > /proc/sysrq-trigger\r")
    return "sysrq panic", True


def flight(args, c, lx):
    secs = 60
    check("flight: Linux has /dev/ttyRPMSG0", "/dev/ttyRPMSG0" in lx.run("ls /dev/ttyRPMSG0 2>&1", 10))

    # Step 8: the link. mavlink_link sends a heartbeat every second, which PX4 needs both to learn Linux's
    # endpoint address and, since the link-loss failsafe is on, before it will arm.
    out = lx.run(f"mavlink_link {secs}", secs * 5 + 60)
    m = re.search(r"(\d+) heartbeats, (\d+) attitude", out)
    hb, att = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    check("mavlink: heartbeat about 1 Hz", hb >= secs - 3, f"{hb} in {secs} s")
    check("mavlink: ATTITUDE about 25 Hz", att >= secs * 20, f"{att} in {secs} s")
    check("mavlink: no CRC errors, no sequence gaps", "crc errors 0, sequence gaps 0" in out,
          (re.findall(r"crc errors.*", out) or ["no result"])[-1])

    # Step 7: the closed loop, commanded from Linux over MAVLink. The program stays up for the rest of the
    # flight: with NAV_DLL_ACT 3 PX4 lands 5 s after the last heartbeat, and a second one is the link
    # the isolation checks below rely on.
    lx.run("(mavlink_link 3000 takeoff > /tmp/ml.log 2>&1 &)", 10)
    z = None
    for _ in range(40):
        time.sleep(3)
        z = vehicle(c)[0]
        if z is not None and z < -2.4:
            break
    log = lx.run("cat /tmp/ml.log", 10)
    check("mavlink: arm and takeoff commanded from Linux, both acknowledged",
          "arm ack 0" in log and "takeoff ack 0" in log and z is not None and z < -2.4,
          f"z={z}; " + "; ".join(re.findall(r"(?:arm|takeoff) ack -?\d+", log)))
    time.sleep(10)
    out = c.run("ekf2 status")
    check("sih: EKF2 running, attitude and position valid",
          "attitude: 1, local position: 1, global position: 1" in out,
          (re.findall(r"EKF dt.*", out) or ["no status"])[-1])
    z0, t0 = vehicle(c)
    wall0 = time.time()
    zs = [z0]
    for _ in range(6):
        time.sleep(5)
        zs.append(vehicle(c)[0])
    z1, t1 = vehicle(c)
    zs = [z for z in zs if z is not None]
    check("sih: hover at 2.5 m holds", bool(zs) and all(hovering(z) for z in zs),
          f"z {min(zs):.2f}..{max(zs):.2f} m" if zs else "no samples")
    check("sih: armed, no failsafe", commander(c)[1:] == (False, True), str(commander(c)))
    if t0 and t1:
        print(f"      simulated {t1 - t0:.1f} s in {time.time() - wall0:.1f} s of wall clock "
              f"({(t1 - t0) / (time.time() - wall0):.2f}x)")
    cpu = re.search(r"CPU usage: ([\d.]+)% tasks", c.run("top once"))
    if cpu:
        print(f"      CPU: {cpu.group(1)}% busy while hovering with MAVLink streaming")

    # Isolation (a): Linux under full load must not touch the flight
    c.run("perf reset")
    time.sleep(20)
    base = late_callouts(c)
    lx.run(f"(stress-ng --cpu 3 --vm 2 --vm-bytes 32M --timeout {args.stress_secs}s > /tmp/stress.log 2>&1 &)", 10)
    time.sleep(5)
    procs = lx.run("ps | grep -c '[s]tress-ng'", 10).strip().splitlines()[-1:]
    check("isolation: stress-ng running on the Linux harts", bool(procs) and procs[0].isdigit() and int(procs[0]) >= 4,
          f"{procs[0] if procs else '?'} processes")
    c.run("perf reset")
    zs = []
    end = time.time() + args.stress_secs - 10
    while time.time() < end:
        time.sleep(5)
        zs.append(vehicle(c)[0])
    zs = [z for z in zs if z is not None]
    check("isolation: hover holds under stress-ng", bool(zs) and all(hovering(z) for z in zs),
          f"z {min(zs):.2f}..{max(zs):.2f} m" if zs else "no samples")
    check("isolation: no failsafe under stress-ng", commander(c)[1:] == (False, True), str(commander(c)))
    loaded = late_callouts(c)
    check("isolation: no more late HRT callouts under stress-ng", loaded <= base, f"{base} idle, {loaded} loaded")
    time.sleep(15)   # let stress-ng finish

    # Isolation (b): Linux gone. PX4 must notice (COM_DL_LOSS_T 5 s), land (NAV_DLL_ACT 3) and disarm by itself.
    how, confirmed = freeze_linux(args, lx)
    check(f"link loss: Linux is really gone ({how})", confirmed)
    _, sim0 = vehicle(c)
    wall0, t_fail, landed, el = time.time(), None, False, 0.0
    while time.time() - wall0 < 900:
        time.sleep(3)
        mode, failsafe, armed = commander(c)
        z, sim = vehicle(c)
        el = (sim - sim0) if sim and sim0 else float("nan")
        print(f"      link lost +{el:5.1f} s: mode={mode} failsafe={failsafe} armed={armed} z={z}")
        if failsafe and t_fail is None:
            t_fail = el
        if not armed and z is not None and z > -0.3:
            landed = True
            break
        if el > 180:
            break
    check(f"link loss ({how}): PX4 declares the data link lost within 15 s", t_fail is not None and t_fail < 15,
          f"after {t_fail:.1f} s" if t_fail is not None else "never")
    check("link loss: PX4 lands and disarms on its own", landed, f"after {el:.0f} s")
    if t_fail is None or not landed:
        # what PX4 thinks of the link, and whether Linux is really gone
        print("      --- diagnostics ---")
        print(c.run("mavlink status")[:900])
        print(c.run("listener failsafe_flags -n 1")[:1200])
        if args.target == "renode":
            mon = Monitor(args.monitor)
            for h in (2, 3, 4):
                print(f"      u54_{h}: {mon.cmd(f'sysbus.u54_{h} IsHalted').strip().splitlines()[-1:]}")
            mon.close()


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
    ap.add_argument("--flight", action="store_true", help="steps 7-8: SIH flight with Linux as companion (needs --linux)")
    ap.add_argument("--stress-secs", type=int, default=60, help="--flight: stress-ng duration")
    args = ap.parse_args()
    if args.flight and not args.linux:
        ap.error("--flight needs --linux")

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
    # usleep(100 ms) rounds up to whole 1 ms NuttX ticks plus one, so 100-102 ms on an idle board. Since
    # step 7 the flight stack runs (about half the CPU) and preempts this priority-100 NSH task for a few
    # ms, so the upper bound is 110 ms; timer accuracy itself is what `perf latency` below checks.
    check("tests hrt: 100 ms sleeps", len(per) == 10 and all(100000 <= p <= 110000 for p in per),
          f"{min(per)}..{max(per)} us" if per else "no samples")

    # Multi-hart Renode delivers interrupts at time-quantum boundaries, and the hart taking the interrupt
    # can be up to a quantum ahead of the source, so there the bound is 5 us plus two quanta (measured:
    # nothing later than 20 us at a 10 us quantum; see px4-linux.resc). On the board it is 5 us.
    limit = 5
    if args.target == "renode":
        mon = Monitor(args.monitor)
        info = mon.cmd("machine GetTimeSourceInfo")
        mon.close()
        q = re.search(r"Quantum: (\d+):(\d+):([\d.]+)", info)
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

    # A parameter without a board default (SYS_AUTOSTART is 1100 by default since step 7, so setting
    # it to 1100 would never be saved). COM_DISARM_PRFLT only matters when armed on the ground.
    c.run("param set COM_DISARM_PRFLT 20")
    # autosave is deferred, so the saved flag (+) can take a moment to appear
    for _ in range(10):
        out = c.run("param show COM_DISARM_PRFLT")
        saved = re.search(r"x \+\s+COM_DISARM_PRFLT .*: 20", out) is not None
        if saved:
            break
        time.sleep(0.5)
    check("param set/show, saved", saved, re.search(r".*COM_DISARM_PRFLT .*", out).group(0).strip() if "COM_DISARM_PRFLT" in out else "")
    c.run("param set COM_DISARM_PRFLT 10")

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
        mon.close()
        ok = len(samples) == 2 and samples[1][0] > samples[0][0]
        rate = (samples[1][1] - samples[0][1]) / (samples[1][0] - samples[0][0]) if ok else 0
        check("hrt (mtime) advances 1,000,000 per simulated second", ok and abs(rate - 1e6) < 1, f"{rate:.0f}/s")

    if args.flight:
        flight(args, c, lx)

    print(f"\n{sum(results)}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
