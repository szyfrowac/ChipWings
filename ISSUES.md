# Chipwings: issues and open items

Written 2026-11-09, after Phase 5 step 9 (commit `f7b43ff`). Covers steps 7-9 and the items carried over from earlier
steps. Details and measurements are in `sim/README.md` (Phase 5) and `px4-mpfs-plan.html` (section "Progress and
deviations"). All results below are from Renode, not the board.

## Open

### Needs a decision or work

| # | Item | Notes |
|---|------|-------|
| 1 | **`perf latency` check is marginal** | `phase5_test.py` requires 99% of HRT callouts within 25 us with Linux running. With the flight stack running it measured 98.6%-99.3% over several runs, so it fails about half the time. The threshold is unchanged on purpose. Options: measure it with the flight stack idle, raise the quantum allowance, or accept a lower figure and say why. |
| 2 | **Phase 2 Linux guide has no Phase 5 results** | The Claude Docs page "Linux (Phase 2) porting guide" was last updated for Phase 4. Updating it edits a shared doc, so it was not done without asking. |
| 3 | **Published plan is out of date** | Only the local `px4-mpfs-plan.html` has the new "Progress and deviations" section. The published artifact (rev 3) was not touched. |
| 4 | **`--target board` untested** | `rpmsg_test.py`, `run_examples.py` and `phase5_test.py` all take `--target board` through `sim/common/cwconsole.py`. Verified on Renode only (rpmsg_test 500/500, examples 6/6); the serial path has not run. |
| 5 | **HSS boot rehearsal in Renode not done** | Plan addition B. Renode skips HSS: it loads images directly and uses our own IHC model and OpenSBI extension, so the HSS boot path is untested until the board. `sim/images/icicle-hss.elf` exists but was not used. |
| 6 | **MSS watchdog for hart 1** | Planned for Phase 6 ("after step 9"). PX4 has no watchdog on this board yet. |
| 7 | **Hardware steps H0-H6 are blocked** | Power supply had not been delivered as of 2026-10-31 (from earlier notes). No serial device (`/dev/ttyUSB*`, `/dev/ttyACM*`) is visible on this machine as of 2026-11-09. |

### Design constraints and risks to remember

| # | Item | Notes |
|---|------|-------|
| 8 | **PX4 now refuses to arm without a data link** | Board defaults `NAV_DLL_ACT 3` and `COM_DL_LOSS_T 5` (needed for the link-loss landing) make PX4 require a GCS-type heartbeat before arming. Arming from the NSH shell with no MAVLink peer fails. **Hardware step H3 ("PX4 alone") will hit this**: set `NAV_DLL_ACT 0` for that step or provide a MAVLink peer. |
| 9 | **Linux must speak first, and keep speaking** | `rpmsg_tty.c` cannot send until Linux has sent a message (OpenAMP learns Linux's endpoint address from it) and drops output whenever Linux has been silent for 3 s. Fine for MAVLink (1 Hz heartbeat); any other client of `rpmsg-tty` must do the same. |
| 10 | **IHC notify retry budget is now about 1 ms** | NuttX patch 0005. No lost kicks seen (2000/2000 RPMsg round trips, stress-ng, 60 s MAVLink streams), but a Linux that is alive and slower than 1 ms to acknowledge would now lose a kick. A deferred retry (work queue) was considered and not needed so far. Real timing on the board is not measured. |
| 11 | **Cost of a dead Linux is not zero** | In the 3 s before the liveness gate closes, 255 notifications failed at about 1 ms each (frozen-harts test). The MAVLink thread runs at NSH's priority, so the shell is briefly starved; flight tasks (higher priority) were not affected. Measure on the board at H4. |
| 12 | **Renode does not model L2/DDR contention** | The isolation results (hover holds under stress-ng, no late callouts) show the software does not depend on Linux, not real latency. Repeat at H4. |
| 13 | **A few HRT callouts take 100-1000 us** | Persistent in every run: about 16-34 of 100,000-200,000 callouts (0.02-0.03%), none above 1 ms. First seen at step 6 as "1 callout over 100 us". Not investigated; may be Renode scheduling. |
| 14 | **`board_reset` resets the whole chip** | On hardware it would reboot Linux too. Ask HSS for a hart-1-only restart (plan risk). |
| 15 | **Linux can write PX4's memory unless PMP stops it** | HSS PMP configuration should leave Linux no write access outside the shared RPMsg region. Verify at H2 (plan risk). |
| 16 | **IHC v1 vs rtl-v2** | Check at H0 which IHC version the 2025 reference design has; our Renode model and the NuttX driver assume one of them. |
| 17 | **`CONFIG_RPMSG_UART=y` is set only for its side effect** | It selects the serial TX/RX DMA support `rpmsg_tty.c` uses. The unused NuttX `uart_rpmsg.c` is therefore linked in, and `rpmsg_serialinit()` is an empty stub in `rpmsg_tty.c`. Works, but is a workaround. |

### Test and tooling caveats

| # | Item | Notes |
|---|------|-------|
| 18 | **Run `phase5_test.py` once per boot** | The dmesg check reads the boot lines, which scroll out of the 32 KiB buffer. `--flight` also leaves the vehicle disarmed and Linux gone. |
| 19 | **A panicked Linux slows Renode to about 0.03x** | The spinning harts dominate. `isolation_test.py` (sysrq panic) therefore only checks that PX4 keeps flying and the shell comes back (about 117 s wall clock before patch 0005, 34 s after). Timing checks use frozen harts instead. |
| 20 | **Renode's telnet monitor serves one connection at a time** | A second connection is accepted and its commands are silently ignored. Always close `Monitor` sockets. `phase4/run.sh` has no `stop`: stop that instance through the monitor (`quit` on port 33456). |
| 21 | **Test thresholds changed for the flight stack** | 100 ms sleep bound raised from 102 ms to 110 ms (flight tasks preempt the NSH task); the param check now uses `COM_DISARM_PRFLT` because `SYS_AUTOSTART` defaults to 1100. |
| 22 | **Expected log noise** | "command not found" for modules not built (`tone_alarm`, `rgbled`, `px4io`, `rc_input`, ...), `UAVCAN_ENABLE` and `GPS_1_CONFIG` not found, `parameters: get: param 65535 invalid`. Harmless. |
| 23 | **Stray files in the repo root** | Accidental `.md` files with odd names (`*_.md`, `if(NOT_USE_LD_GOLD).md`, `PX4-Autopilot.md`, ...), `v1.18.0-beta1-1111-g2114fe2f78.md`, and the contest PDF. Noticed at the start; not cleaned up. |
| 24 | **Commit rules differ inside the PX4 clone** | `sw/px4/AGENTS.md` asks for an `Assisted-by:` trailer, no `Co-Authored-By`, conventional-commit messages and `make format`. Our port files live outside the clone and are committed in this repo, so they do not apply; they would if changes go upstream. |

## Found and fixed in steps 7-9

| Problem | Cause | Fix |
|---------|-------|-----|
| NuttX assertion in `mm_mallinfo.c:72` when SIH starts | Modules hard-code stack sizes for 32-bit ARM; `sih` asked for 1560 bytes and overflowed into the heap on RV64 | PX4 patch 0006 doubles task stacks for 64-bit pointers (as PX4's POSIX layer already does) |
| NULL function pointer in `uart_open`, then a boot assertion in `group_setupidlefiles` | NuttX's make does not rebuild on a config change, so old objects (`drivers/serial.o` from Oct 31) kept the old `uart_dev_s` layout | Deleted the stale objects; `build-px4.sh` now clears NuttX objects when the board defconfigs change |
| Link error `rpmsg_serialinit` | `CONFIG_RPMSG_UART` makes `drivers_initialize()` call it | Empty stub in `rpmsg_tty.c` |
| ATTITUDE only 10 Hz | `mavlink -r 20000` byte budget; onboard streams need about 36 kB/s | `-r 100000` |
| Burst of stale telemetry, then a sequence gap, when Linux attached | Output was queued while Linux was silent, then overflowed | Output is discarded unless Linux is alive (heard within 3 s) |
| PX4 shell hung after Linux died | Every IHC notify spun up to 1000 x 10,000 register reads (about 1 s on the board) in the MAVLink thread | Liveness gate in `rpmsg_tty.c` plus NuttX patch 0005 (about 1 ms budget) in both NuttX trees |
| Patch 0005 would not apply to PX4's NuttX fork | `arch/risc-v/src/chip` is a symlink there; git refuses to patch through it | Patch uses the real `mpfs` path, like 0003 and 0004 |
| Link-loss test failed ("never declared") | Test bug: a leaked monitor connection made the freeze command a silent no-op | Monitor sockets closed; the suite reads `IsHalted` back and fails with "Linux is really gone" |
| Link-loss test looked 220 s late | Renode ran at 0.03x after a sysrq panic; the failsafe timing was correct in simulated time | Link loss now uses frozen harts, normal speed |
| Vehicle landed before the hover checks | Test bug: the 60 s `mavlink_link` run ended, heartbeats stopped, PX4 correctly landed | One long-running `mavlink_link` for the whole flight |
| Isolation test could not arm | Arming now needs a data link (item 8) | Test starts `mavlink_link` first |
| Weak checks in my own tests | A vacuous `or True` check; a `loadavg` parse that grabbed the wrong line | Replaced with real checks; added "stress-ng running" |
| `param set/show, saved` failed | `SYS_AUTOSTART` became 1100 by default | Test uses a parameter without a board default |
