#!/usr/bin/env python3
"""Phase 5 step 8: MAVLink between PX4 (hart 1) and Linux (harts 2-4) over /dev/ttyRPMSG0.

Start first:  sim/phase5/run.sh headless linux     (wait until Linux shows its login prompt)
Run:          sim/phase5/mavlink_test.py [--secs 60]

  1. mavlink_link <secs>: HEARTBEAT at 1 Hz, ATTITUDE present, no CRC errors, no sequence gaps
  2. mavlink_link <secs> takeoff: Linux arms and commands takeoff over MAVLink, z reaches about -2.5 m
  3. PX4's own view: `mavlink status` shows the RPMsg instance, the vehicle is armed in the air
  4. link loss: Linux keeps sending heartbeats, then harts 2-4 are frozen from the Renode monitor (a hung
     Linux); PX4 must declare the data link lost (NAV_DLL_ACT 3 = land, COM_DL_LOSS_T 5 s) and land and
     disarm by itself. A sysrq panic is not used here: panicked harts spin and Renode drops to ~0.03x real
     time, so the check would take hours of wall clock (isolation_test.py covers the panic).
"""
import argparse, os, re, socket, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script dir
from isolation_test import Console, z_now, nav_state

def freeze_linux(port):
    """Halt harts 2-4 in Renode: Linux stops dead, the way a hard hang would."""
    m = socket.create_connection(("localhost", port))
    for h in (2, 3, 4):
        m.sendall(f"sysbus.u54_{h} IsHalted true\n".encode())
        time.sleep(0.5)
    time.sleep(1)
    m.close()

def sim_seconds(nsh):
    out = nsh.run("listener vehicle_local_position -n 1")
    m = re.search(r"^\s+timestamp: (\d+)", out, re.M)
    return int(m.group(1)) / 1e6 if m else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=60)
    ap.add_argument("--nsh-port", type=int, default=33457)
    ap.add_argument("--linux-port", type=int, default=33458)
    ap.add_argument("--monitor-port", type=int, default=33456)
    a = ap.parse_args()
    nsh, lin = Console(a.nsh_port, "px4"), Console(a.linux_port, "linux")
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    nsh.run("echo sync")
    lin.s.sendall(b"\n"); time.sleep(1)
    if "login:" in lin.read(2):
        lin.s.sendall(b"root\n"); time.sleep(1); lin.read(1)
        lin.s.sendall(b"root\n"); time.sleep(1); lin.read(1)
    lin.run("echo linux-ready", 30)
    check("Linux has /dev/ttyRPMSG0", "/dev/ttyRPMSG0" in lin.run("ls /dev/ttyRPMSG0 2>&1", 10))

    out = lin.run(f"mavlink_link {a.secs}", a.secs * 5 + 60)
    print(out.strip())
    check("60 s stream: mavlink_link PASS", "mavlink_link: PASS" in out)
    m = re.search(r"(\d+) heartbeats, (\d+) attitude", out)
    if m:
        check("heartbeat rate ~1 Hz", int(m.group(1)) >= a.secs - 3, f"{m.group(1)} in {a.secs} s")
        check("ATTITUDE rate ~25 Hz", int(m.group(2)) >= a.secs * 20, f"{m.group(2)} in {a.secs} s")
    check("no CRC errors or sequence gaps", "crc errors 0, sequence gaps 0" in out)

    out = lin.run(f"mavlink_link {a.secs} takeoff", a.secs * 5 + 60)
    print(out.strip())
    check("takeoff commanded from Linux", "mavlink_link: PASS" in out)
    z = z_now(nsh)
    st = nav_state(nsh)
    check("PX4 reports the vehicle at 2.5 m, armed", z is not None and z < -2.3 and st[2], f"z={z} {st}")
    status = nsh.run("mavlink status")
    check("mavlink status lists /dev/ttyRPMSG0", "ttyRPMSG0" in status)

    # 4. link loss
    lin.s.sendall(b"mavlink_link 900 > /tmp/ml.log 2>&1 &\n")
    time.sleep(20)
    check("hovering with the link up", abs((z_now(nsh) or 0) + 2.5) < 0.2)
    freeze_linux(a.monitor_port)
    sim0, wall0 = sim_seconds(nsh), time.time()
    seen_failsafe, landed, t_fs = False, False, None
    while time.time() - wall0 < 600:
        time.sleep(3)
        mode, fs, armed = nav_state(nsh)
        z, sim = z_now(nsh), sim_seconds(nsh)
        el = (sim - sim0) if sim and sim0 else float("nan")
        print(f"  sim t+{el:5.1f}s (wall {time.time() - wall0:3.0f}s) mode={mode} failsafe={fs} armed={armed} z={z}")
        if fs and not seen_failsafe:
            seen_failsafe, t_fs = True, el
        if not armed and z is not None and z > -0.3:
            landed = True
            break
        if el > 180:
            break
    check("PX4 declared the link lost (failsafe) within 15 s", seen_failsafe and t_fs < 15, f"after {t_fs} s")
    check("PX4 landed and disarmed on its own", landed, f"after {el:.0f} s")

    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)

if __name__ == "__main__":
    main()
