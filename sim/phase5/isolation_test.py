#!/usr/bin/env python3
"""Phase 5 step 7 isolation checks: PX4 hovering in SIH must not depend on Linux.

Start first:  sim/phase5/run.sh headless linux     (wait until Linux shows its login prompt)
Run:          sim/phase5/isolation_test.py [--stress-secs 60]

  0. log in to Linux and start mavlink_link: PX4 refuses to arm without a data link (NAV_DLL_ACT 3 is a
     board default since step 8), and the MAVLink traffic is part of the load PX4 must shrug off
  1. arm + takeoff on PX4 (NSH, telnet 33457), wait for z near -2.5 m
  2. baseline: `perf reset`, hover, `perf latency`
  3. stress-ng on the three Linux harts (telnet 33458, root/root): hover must hold, latency must not shift
  4. echo c > /proc/sysrq-trigger (kernel panic): PX4 must stay alive and keep flying: still hovering, or
     already landing on its own link-loss failsafe. Panicked harts spin and Renode drops to ~0.03x real
     time, so only a few simulated seconds pass here; the full link-loss landing is tested in
     mavlink_test.py (frozen harts, normal speed).
"""
import argparse, random, re, socket, sys, time

class Console:
    def __init__(self, port, name):
        self.s = socket.create_connection(("localhost", port))
        self.s.settimeout(0.5)
        self.name = name

    def read(self, secs):
        end, buf = time.time() + secs, b""
        while time.time() < end:
            try:
                buf += self.s.recv(65536)
            except socket.timeout:
                pass
        return buf.decode(errors="replace")

    def run(self, cmd, timeout=60):
        """Run a shell command, return its output (between echoed command and end marker)."""
        tag = "__DONE%d" % random.randrange(10**8)
        self.s.sendall(f"{cmd}\necho {tag} 42\n".encode())
        out, end = "", time.time() + timeout
        while time.time() < end:
            out += self.read(0.5)
            m = re.search(rf"^{tag} 42", out, re.M)
            if m:
                return out[:m.start()]
        raise TimeoutError(f"{self.name}: no end marker for {cmd!r}")

def z_now(nsh, timeout=60):
    out = nsh.run("listener vehicle_local_position -n 1", timeout)
    m = re.search(r"^\s+z: (-?[\d.]+)", out, re.M)
    return float(m.group(1)) if m else None

def nav_state(nsh, timeout=60):
    out = nsh.run("commander status", timeout)
    mode = re.search(r"navigation mode: (\w+)", out)
    return (mode.group(1) if mode else "?", "in failsafe: yes" in out, "Armed" in out and "Disarmed" not in out)

def latency(nsh):
    out = nsh.run("perf latency")
    b = {}
    for m in re.finditer(r"^\s*(\d+) : (\d+)", out, re.M):
        b[int(m.group(1))] = int(m.group(2))
    over = re.search(r">\s*1000 : (\d+)", out)
    b[">1000"] = int(over.group(1)) if over else 0
    return b

def slow(b):
    """Callouts later than 100 us."""
    return b.get(1000, 0) + b.get(">1000", 0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stress-secs", type=int, default=60)
    ap.add_argument("--nsh-port", type=int, default=33457)
    ap.add_argument("--linux-port", type=int, default=33458)
    a = ap.parse_args()

    nsh = Console(a.nsh_port, "px4")
    lin = Console(a.linux_port, "linux")
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    nsh.run("echo sync")

    # Linux login, then the companion link PX4 needs before it will arm
    lin.s.sendall(b"\n")
    time.sleep(1)
    txt = lin.read(2)
    if "login:" in txt:
        lin.s.sendall(b"root\n"); time.sleep(1); lin.read(1)
        lin.s.sendall(b"root\n"); time.sleep(1); lin.read(1)
    lin.run("echo linux-ready", 30)
    lin.s.sendall(b"mavlink_link 3000 > /tmp/ml.log 2>&1 &\n")
    time.sleep(15)

    print(nsh.run("commander arm"), end="")
    nsh.run("commander takeoff")
    z = None
    for _ in range(40):
        time.sleep(3)
        z = z_now(nsh)
        if z is not None and z < -2.4:
            break
    check("takeoff reaches 2.5 m", z is not None and z < -2.4, f"z={z}")
    time.sleep(10)

    # baseline
    nsh.run("perf reset")
    time.sleep(30)
    base = latency(nsh)
    zb = z_now(nsh)
    print(f"baseline latency {base}")
    check("baseline hover", zb is not None and abs(zb + 2.5) < 0.2, f"z={zb}")

    # (a) stress
    lin.s.sendall(f"stress-ng --cpu 3 --vm 2 --vm-bytes 32M --timeout {a.stress_secs}s &\n".encode())
    time.sleep(2)
    nsh.run("perf reset")
    zs = []
    t_end = time.time() + a.stress_secs - 5
    while time.time() < t_end:
        time.sleep(5)
        zs.append(z_now(nsh))
    load = lin.run("cat /proc/loadavg; ps | grep -c '[s]tress-ng'")
    stress_lat = latency(nsh)
    lines = [l for l in load.splitlines() if re.match(r"^\d+\.\d+ |^\d+$", l.strip())]
    print(f"stress latency   {stress_lat}\nlinux loadavg / stress-ng procs: {lines}")
    nproc = int(lines[-1]) if lines and lines[-1].strip().isdigit() else 0
    check("stress-ng running on Linux", nproc >= 4, f"{nproc} processes")
    zs = [z for z in zs if z is not None]
    check("hover holds under stress-ng", bool(zs) and all(abs(z + 2.5) < 0.2 for z in zs),
          f"z min/max = {min(zs):.2f}/{max(zs):.2f}" if zs else "no samples")
    state = nav_state(nsh)
    check("no failsafe under stress-ng", not state[1], f"{state}")
    check("callouts > 100 us under stress: none more than baseline",
          slow(stress_lat) <= max(slow(base), 0), f"base={slow(base)} stress={slow(stress_lat)}")

    # (b) Linux panic
    lin.run("sleep 0", 10)
    nsh.run("perf reset")
    lin.s.sendall(b"echo c > /proc/sysrq-trigger\n")
    time.sleep(5)
    panic = lin.read(5)
    check("Linux panicked", "sysrq" in panic.lower() or "panic" in panic.lower() or "oops" in panic.lower(),
          panic.strip().splitlines()[-1][:80] if panic.strip() else "no console output")
    # After the panic the MAVLink thread spends a few simulated seconds in IHC notify retries (the peer
    # never acknowledges) before the link is declared down; that can starve the NSH task, so allow the
    # shell a long wall-clock time to answer, and report how long it stayed silent in simulated time.
    t_panic = time.time()
    zp = []
    for _ in range(4):
        time.sleep(5)
        zp.append(z_now(nsh, timeout=900))
    print(f"PX4 shell answered {time.time() - t_panic:.0f} s (wall) after the panic")
    zp = [z for z in zp if z is not None]
    state = nav_state(nsh, timeout=900)
    hovering = bool(zp) and all(abs(z + 2.5) < 0.2 for z in zp)
    landing = state[1] and state[0] in ("Land", "Descend")   # link-loss failsafe already acting
    check("PX4 still flying after Linux panic (hover or failsafe landing)", state[2] and (hovering or landing),
          f"z min/max = {min(zp):.2f}/{max(zp):.2f} {state}" if zp else "no samples")
    after = latency(nsh)
    print(f"post-panic latency {after}")
    check("PX4 shell responsive after panic", "vehicle_local_position" in nsh.run("uorb top -1", 900))

    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)

if __name__ == "__main__":
    main()
