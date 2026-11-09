# Renode simulation of the single-chip UAV avionics SoC

Renode 1.17.0 (portable build) is installed at `~/tools/renode_1.17.0-portable/`.
Prebuilt demo images live in `images/` (fetched from dl.antmicro.com, SHA1-verified).

## Phase 0 — stock PolarFire SoC demos

| Script | What it runs | Output |
|---|---|---|
| `phase0/freertos-smoke.resc` | FreeRTOS + lwIP web server on the PSE, 3 s virtual time, then quits | `phase0/logs/freertos-mmuart0.log` |
| `phase0/icicle-linux.resc` | HSS (E51) -> U-Boot -> Linux 5.6 SMP on U54 harts 1-4, Buildroot userspace | `phase0/logs/icicle-mmuart0-hss.log`, `phase0/logs/icicle-mmuart1-linux.log` |

```sh
R=~/tools/renode_1.17.0-portable/renode

# FreeRTOS smoke test (~35 s on this laptop)
$R --console --disable-gui sim/phase0/freertos-smoke.resc < /dev/null

# Linux boot (login prompt after ~35 s host time); monitor on telnet port 33456
$R --disable-gui -P 33456 sim/phase0/icicle-linux.resc
# Interactive Linux console: in the monitor (telnet localhost 33456) run
#   emulation CreateServerSocketTerminal 33457 "term1" false
#   connector Connect sysbus.mmuart1 term1
# then `telnet localhost 33457`, log in as root/root.
# Or just run Renode with the GUI and add `showAnalyzer mmuart1`.
```

## Gotchas found while setting this up

- **Paths in .resc:** use `$ORIGIN/...` (the script's directory) without `@`; `@$ORIGIN/...` is not expanded, and plain relative paths are resolved against Renode's directory, not the script's.
- **Don't `include` the stock demo scripts:** Renode fetches every `$var?=@https://...` URL in them even if you set the variable first. Our scripts inline the stock setup instead.
- **`logLevel 3`:** the PolarFire model logs huge numbers of WARNINGs (7.6 GB in 3 s for the FreeRTOS demo, and it slowed the run ~5x).
- **Headless runs:** `--console` with stdin at EOF makes Renode quit after the script finishes, which is fine for scripts ending in `q` but not for long-running sessions; use `-P <port>` for those. Monitor errors only show up with `--console` (or on the telnet monitor).
- **Network:** downloads from GitHub/antmicro sometimes get TLS resets here; `curl --retry 5 --retry-all-errors` works.
- The demo images date from 2020 (HSS 0.99.7, Linux 5.6) and put Linux on all four U54s. From Phase 1 on we build our own images: NuttX on hart 1, Linux on harts 2-4.

## Phase 1 — NuttX alone on U54 hart 1

Toolchain: xPack `riscv-none-elf-gcc` 15.2.0-1 at `~/tools/xpack-riscv-none-elf-gcc-15.2.0-1/bin` (NuttX picks the `riscv-none-elf-` prefix automatically).
Sources: `sw/nuttx` + `sw/apps`, tag `nuttx-13.1.0`, with `sw/patches/0001-mpfs-timerisr-fix-double-hart-offset-for-mtimecmp.patch` applied.

```sh
sw/build-nuttx.sh                 # patches + chipwings apps + sw/configs/nsh-hart1, ~10 s, links at 0x80000000

sim/phase1/run.sh                 # GUI: monitor window + MMUART1 terminal with NSH
sim/phase1/run.sh headless        # NSH on telnet localhost 33457, monitor on 33456
sim/phase1/check-one-core.sh      # per-core halted state / instruction counts from Renode

# A second instance next to a running one (other ports, own UART log):
renode --disable-gui -P 33466 -e "\$nshport=33467; \$uartlog=@$PWD/uart2.log; i @$PWD/sim/phase1/nuttx-hart1.resc; start"   # run from the project root
sim/phase1/check-one-core.sh 33466
```

Proving NuttX runs on exactly one core:
- In NSH, `hartinfo` (our app in `sw/chipwings-apps/hartinfo`) prints `Physical hart (mhartid) : 1 ... U54_1`, `NuttX SMP : disabled`.
  Don't use `/proc/cpuinfo` for this: NuttX numbers harts from 0 there, so it says `hart : 0` even on hart 1.
- `check-one-core.sh` shows only `u54_1` executing instructions; e51 and u54_2-4 are halted with 0 instructions at PC 0x1000.

`phase1/nuttx-hart1.resc` halts the E51 and U54 harts 2-4 and loads the ELF onto `u54_1` directly (no HSS yet).
Result: `ostest` passes (all 30 groups, exit status 0) in ~4 min host time.

**Bug found with Renode (affects real hardware too):** in NuttX 13.1.0 `mpfs_timerisr.c` passes
`MTIMECMP0 + hart_id*8` to `riscv_mtimer_initialize()`, which adds `8 * up_cpu_index()` again, so NuttX on hart 1
armed **hart 2's** `mtimecmp` (0x02004010). Its own tick never fired (`g_system_ticks == 0`, first `usleep()` hangs),
and on the board it would also have been firing timer interrupts at the Linux hart. Fixed by passing the base address.
Upstream master still has the offset in `mpfs_timerisr.c` — worth reporting.

More gotchas:
- Renode 1.17 `LoadELF` signature is `sysbus LoadELF <file> <useVirtualAddress> <cpu>`; it also sets that CPU's PC.
- With a telnet monitor (`-P`), script errors are only visible on the monitor; connect and re-run the failing line to see them.
- Debugging: `machine StartGdbServer 3333 false u54_1`, then `riscv-none-elf-gdb sw/nuttx/nuttx -ex 'target remote :3333'`.
  GDB's CSR numbering is wrong for this core (it showed a bogus `mtvec`); read CSRs in the monitor instead with
  `u54_1 GetRegisterUnsafe <65 + csr>` (e.g. 838 = mtvec, 901 = mip, 837 = mie).
- The 187 `plic: Trying to complete irq ...` errors at boot are NuttX clearing all claims during init (`mpfs_plic.c`); harmless.
  They can't be muted: `logLevel` only goes up to 3 (ERROR), and `logLevel 4 plic` aborts the script.

## Phase 2 — Linux SMP alone on U54 harts 2-4

Buildroot 2026.02.3 (LTS) in `sw/buildroot`; our external tree `sw/br2-chipwings` (defconfig `chipwings_mpfs_linux_defconfig`,
device tree `board/mpfs-linux/dts/microchip/chipwings-mpfs-linux.dts`); output in `sw/br2-out`.
Kernel: Microchip's `linux4microchip+fpga-2025.10` (6.12.48, `mpfs` defconfig), the same kernel as the Icicle Kit.
Bootlin prebuilt glibc toolchain, BusyBox rootfs built into the kernel as an initramfs, OpenSBI 1.7 `fw_jump`.

```sh
sw/build-linux.sh                       # first build ~5 min on this laptop (24 cores)
sw/build-linux.sh linux-rebuild all     # after changing the device tree or kernel config

sim/phase2/run.sh                       # GUI: monitor + terminal on MMUART3 (Linux console)
sim/phase2/run.sh headless              # console: telnet localhost 33458 (root/root); monitor 33456
sim/phase1/check-one-core.sh            # same per-core check: only u54_2..4 run, e51 and u54_1 stay at 0
```

Boot layout (no HSS/U-Boot in Renode): OpenSBI `fw_jump.elf` at 0x80000000 on harts 2-4 (a1 = DTB), kernel `Image` at
0x80200000, DTB at 0x8A000000. Login prompt after ~5 s host time. In Linux: `nproc` = 3, `/proc/cpuinfo` harts 2,3,4,
`/proc/iomem` shows `a2000000-a403ffff : Reserved` (NuttX + RPMsg).

Problems found and fixed on the way:
- **CLINT slot mapping (affects real AMP hardware too):** `mpfs.dtsi` lists the E51's CLINT interrupts as 0xffffffff, so
  generic OpenSBI took CLINT slot 0 to be hart 1 and every hart used the next hart's registers: hart 2 armed **hart 1's
  (NuttX's) `mtimecmp`** and never cleared its own IPI (interrupt storm, kernel stuck at "Bringing up secondary CPUs").
  Fixed by listing the E51's real interrupts in `&clint`.
- OpenSBI 1.7 generic links at 0x0 (position independent) and jumped to 0x200000: build with `FW_TEXT_START=0x80000000`
  (needs `opensbi-dirclean`, a plain rebuild does not relink).
- RISC-V kernels only build DTBs in vendor directories: our DTS lives in `dts/microchip/` (`BR2_LINUX_KERNEL_CUSTOM_DTS_DIR`).
- The `clkcfg` clock/reset driver reads an MSS PLL Renode doesn't model and put MMUART3 into reset (`0x20002088`),
  hanging the console: disabled in the Renode device tree (on the Icicle Kit HSS sets clocks and resets). `syscontroller`
  (needs its mailbox) is disabled too. MMUART3 runs from a fixed 150 MHz clock.
- Harmless: `riscv-plic: Invalid cpuid for context 2` (the PLIC context of disabled hart 1).

## Phase 3 — AMP: NuttX on hart 1 and Linux on harts 2-4 at the same time

NuttX is relinked into the DDR that the Linux device tree reserves for it (`nuttx@a2000000`, no-map), using config
`sw/configs/nsh-amp-hart1`: the Phase 1 defconfig with `CONFIG_RAM_START=0xa2200000`, plus its own `ld.script`
(progmem 2 MiB at 0xA2000000, sram 1 MiB at 0xA2200000, the same split as the board's `ld-ihc.script`).
`build-nuttx.sh` now uses a config's `ld.script` when it has one (passed as `ARCHSCRIPT=`, which overrides the
board's Make.defs choice) and copies each build to `sw/out/<config>/`. No NuttX source change and no IHC yet.
The Linux images are the unchanged Phase 2 build.

```sh
sw/build-nuttx.sh nsh-amp-hart1         # -> sw/out/nsh-amp-hart1/nuttx, entry 0xa2000000
sw/build-linux.sh                       # only if br2-out/images is missing

sim/phase3/run.sh                       # GUI: monitor + terminals on MMUART1 (NSH) and MMUART3 (Linux)
sim/phase3/run.sh headless              # NSH: telnet localhost 33457; Linux: 33458 (root/root); monitor 33456
sim/phase1/check-one-core.sh            # now: e51 halted, u54_1 in 0xA2xxxxxx (NuttX), u54_2..4 in the kernel
```

`phase3/amp.resc` = Phase 1 + Phase 2 in one machine: only the E51 is halted, `LoadELF` NuttX onto `u54_1` and OpenSBI
onto `u54_2`, kernel + DTB as in Phase 2. Both consoles are logged in `phase3/logs/`. Both prompts appear after ~2 s host time.

Checked:
- NSH `hartinfo`: mhartid 1, SMP disabled; `ps` shows the work queues at 0xa22001xx; `free` 1 MiB heap.
- Linux: `nproc` 3, `/proc/cpuinfo` harts 4, 2, 3, `/proc/iomem` `a2000000-a403ffff : Reserved`, riscv-timer interrupts on all three CPUs.
- `check-one-core.sh`: e51 halted with 0 instructions; all four U54s executing, u54_1 at PC 0xa20xxxxx.
- Isolation under load: three busy loops in Linux (load average 2.96 on harts 2-4) while NSH ran `ostest` on hart 1.
  `ostest` passed, all 32 groups, exit status 0, in 544 s host time (about 4 min alone in Phase 1). Afterwards Linux had no RCU
  stalls, soft lockups or oopses in `dmesg`, riscv-timer counts were even across its three CPUs (61-63k), and both
  uptimes agreed (one shared CLINT `mtime`). NuttX heap use rose by 464 B, the same as in Phase 1.
  This is the first run where the two Phase 1/2 timer fixes (NuttX `mtimecmp` offset patch, Linux `&clint` DT fix)
  are exercised together: each OS gets only its own hart's timer.

## Phase 4 — RPMsg between NuttX (hart 1) and Linux (harts 2-4) over the Mi-V IHC

Linux (`virtio_rpmsg_bus`, remoteproc master) and NuttX (OpenAMP via `rptun`, remote) exchange RPMsg messages:
virtqueues and buffers live in shared DDR at 0xA2400000, notifications go through the Mi-V Inter-Hart
Communication IP (IHC v1) at 0x50000000, as in Microchip's Icicle Kit AMP reference design.

```sh
sw/build-nuttx.sh nsh-rpmsg-hart1       # NuttX with IHC client + rptun, starts RPMsg itself at boot (cwinit)
sw/build-linux.sh                       # OpenSBI with the IHC SBI extension, kernel, DTB with IHC + remoteproc nodes
sim/phase4/run.sh [headless]            # as Phase 3 (NSH 33457, Linux 33458, monitor 33456) + IHC model
sim/phase4/rpmsg_test.py 500            # logs into Linux, binds /dev/rpmsg0 to NuttX's rpmsg-ping, 500 checked round trips
```

What each piece does:

| Piece | Where | Role |
|---|---|---|
| IHC model | `sim/peripherals/MiV_IHC.cs` (compiled by Renode at load) | IHC v1 registers (per hart pair: ctrl, msg in/out; per hart: INT_EN, MSG_AVAIL), level IRQs hart 1 -> PLIC 180, hart 2 -> 179, 3 -> 178, 4 -> 177 |
| OpenSBI extension | `sw/br2-chipwings/patches/opensbi/0001-...` | Microchip vendor SBI extension 0x09000029 (IHC init/tx/rx, rproc state) that HSS provides on the board; context A = hart 2 (lowest OpenSBI hart), context B = hart 1 |
| Linux DT | `chipwings-mpfs-linux.dts` | `microchip,miv-ihc` mailbox (IRQ 179, remote context B), `microchip,miv-remoteproc` with `vdev0buffer` (first, shared-dma-pool), `rsc-table`, `vdev0vring0/1`; NuttX reservation split around them |
| NuttX config | `sw/configs/nsh-rpmsg-hart1` (+ `ld.script`) | `MPFS_IHC_CLIENT`, Linux on harts 2-4 / NuttX on hart 1, `RPTUN`, `RPMSG_PING`, `DEV_SIMPLE_ADDRENV`; `.resource_table` at 0xA2300000 |
| `cwinit` | `sw/chipwings-apps/cwinit` | `CONFIG_INIT_ENTRYPOINT`: `RPTUNIOC_START` on `/dev/rptun/mpfsihc`, then NSH |
| NuttX patches | `sw/patches/0002`, `0003` | resource table at a fixed address; IHC spurious-ack and lost-notification fixes |

Memory (Linux reserved-memory, all no-map): NuttX 0xA2000000 (3 MiB), resource table 0xA2300000 (4 KiB),
vring0 0xA2400000 / vring1 0xA2408000 (32 KiB each), RPMsg buffers 0xA2410000 (256 KiB), the rest up to 0xA403FFFF spare.

Boot sequence: NuttX boots in microseconds, `cwinit` starts rptun, which fills in the resource table; Linux boots,
`miv_remoteproc` asks OpenSBI for the remote state (running), attaches through the resource table, `virtio_rpmsg_bus`
comes up, NuttX announces `rpmsg-ping` (addr 0x401). In Linux: `ls /sys/bus/rpmsg/devices` shows `virtio0.rpmsg-ping.-1.1025`.

Problems found and fixed on the way:
- **No Mi-V IHC in Renode** -> wrote the model. The register semantics come from Microchip's HSS v1 driver: RMP is set by the
  sender and read as MP by the receiver, writing MP=0 consumes, writing ACK=1 sends an ack, writing ACK_CLR=0 clears an incoming
  ack. ACK_CLR reads as 1 so read-modify-write sequences don't clear acks by accident.
- **Linux needs HSS's SBI extension**: `mailbox-miv-ihc.c` only talks to the IHC through SBI calls. Generic OpenSBI's vendor slot
  is 0x09000000 + mvendorid (SiFive 0x489), not Microchip's 0x029, so the module registers its own extension from `final_init`.
- **NuttX's own SBI handler (`mpfs_ihc_sbi.c`) doesn't match the current Linux driver** (irq_type first and 2-word messages vs.
  Linux's 4-word message then irq_type): ported HSS's handler instead.
- **Resource table at an unknown address**: NuttX kept it inside `g_shmem` in .bss. Patch 0002 puts it first and in a
  `.resource_table` section; the linker script pins it at 0xA2300000, the Linux DT points `rsc-table` there.
- **Linux attached to an empty table**: NuttX's rptun doesn't autostart, so the table was still zero when Linux read it ->
  `cwinit` starts it before NSH.
- **RPMsg stalled after 107 round trips** (NuttX bug, affects hardware): the reply was in the vring but Linux never got the kick.
  `mpfs_rptun_notify()` gave up after 5 back-to-back tries when a Linux message was pending, and on every incoming ack the ISR
  also "received" the stale message and acked it. Patch 0003: only treat MP as present when its bit is set; retry the notify for
  ~10 ms (the pending message is cleared by our own ISR) and log if it is ever lost.
- **Linux RCU stall under fork-heavy load (Renode bug, not our software):** a hart sat in OpenSBI's remote TLB flush
  (`tlb_sync`) forever because the hart it was waiting for never took its IPI: CLINT `msip[2]` read 1 but hart 2's
  `mip.MSIP` was 0 (rewriting msip from the monitor un-stuck it). Two races, both from parallel execution (one host
  thread per hart):
  1. Renode's `CoreLevelInterruptor` stores msip and then sets the IRQ line with nothing around the pair, so two harts
     writing the same msip can leave register and line out of step.
  2. The real culprit: `BaseRiscV.OnGPIO` calls `TlibSetMipBit` on the *raising* thread. Hart 3 setting hart 2's MSIP
     races with hart 2 writing its own `mip` (OpenSBI sets `mip.STIP` for the Linux timer), and one update is lost.
     `GPIO.Set` skips repeated values, so the line never re-asserts. Fixing only (1) still stalled after 50 s.

  Fix, in our own platform `sim/platforms/chipwings-mpfs.repl` (stock `polarfire-soc.repl` + these + the IHC):
  - `sim/peripherals/SafeIrqRiscV64.cs`: `RiscV64` whose `OnGPIO` applies a change requested from another thread at
    the nearest synced state (no CPU running, so nobody else touches `mip`); own-thread changes stay immediate.
  - `sim/peripherals/SafeIpiCoreLevelInterruptor.cs`: CLINT whose msip write re-sets the line from the stored value
    until it is stable. (A first version with a lock deadlocked against the timer framework.)

  First workaround was `machine SetSerialExecution true` (boot 24 s -> 41 s); with the fix, scripts run parallel and
  boot to login in ~12 s. Verified: two `fork` loops on Linux + RPMsg round trips + NuttX `ostest` ran to 810 s guest
  time with no stall (CLINT-only fix: stall at 50 s; stock: 399 s), `ostest` passed, kernel log clean.
- Kernel picks `vdev0buffer` only via `memory-region` index 0 (miv_remoteproc skips it when building carveouts).
- OpenAMP/libmetal are downloaded by the NuttX build; pre-downloaded here, unpacked + patched once, and `git init`ed in
  `sw/nuttx/openamp/{open-amp,libmetal}` so `make distclean` keeps them.

Note: the Phase 2/3 scripts now boot the same Linux images, which include the remoteproc node; with the Phase 3 NuttX (no rptun)
Linux attaches to an all-zero resource table and simply has no RPMsg channel.

## Phase 5 — PX4 on U54 hart 1 (in progress)

Plan (rev 3, includes the Icicle Kit validation ladder H0-H6): [`../px4-mpfs-plan.html`](../px4-mpfs-plan.html)

PX4 `main` at 5f429033 in `sw/px4` (PX4's NuttX fork e5a23078). Port files live outside the clone in
`sw/px4-chipwings/` (copied over the tree) and `sw/px4-patches/` (applied once); `sw/build-px4.sh` does both and builds.

Setup (step 1):
- Python: `sw/px4-venv`, from `Tools/setup/requirements.txt`. `python3-venv` (ensurepip) is not installed and needs sudo,
  so the venv was made with `python3 -m venv --without-pip` and pip bootstrapped with `get-pip.py`.
- Submodules needed beyond NuttX: `src/modules/mavlink/mavlink`, `src/lib/events/libevents`.
- Toolchain: PX4 hard-codes `riscv64-unknown-elf-`. `build-px4.sh` generates `sw/out/px4-toolchain/` with
  `riscv64-unknown-elf-*` wrapper scripts that exec the xPack `riscv-none-elf-*` 15.2 tools (wrappers, not symlinks: GCC
  finds cc1 and multilibs relative to its own path). `./build-px4.sh --toolchain` checks compiler, ABI and venv.
- `PATH` puts `/usr/bin` before Vivado's directory, which ships its own `ninja`.

Chip layer and board (steps 2-4, done 2026-10-31; `./build-px4.sh` links `px4.elf`, 491 KiB code, no warnings):
- Layout copies STM32: `platforms/nuttx/src/px4/microchip/{microchip_common,mpfs}` like `stm/{stm32_common,stm32h7}`,
  board `boards/chipwings/mpfs-hart1` like `boards/px4/fmu-v6x`. Pieces now: `hrt`, `board_reset`, `version`,
  `board_critmon`, `include/px4_arch/{micro_hal,hw_description,spi_hw_description,i2c_hw_description}.h`.
- PX4 patches: 0001 chip detection (`CONFIG_ARCH_CHIP_MPFS` -> microchip/mpfs), 0002 `PX4_SOC_ARCH_ID_MPFS = 0x000A`,
  0003 `controllib_test` did not declare controllib/mathlib (only links on boards where another module pulls them in).
- PX4's NuttX build compiles no NuttX board sources (`boards/Makefile` builds `dummy.c`), so the icicle hooks
  (`mpfs_boardinitialize`, `board_app_initialize`, later the IHC/rptun start) live in the board's `src/init.cpp`.
- hrt: time = CLINT mtime (1 MHz, 64 bit, no wrap handling); deadlines = MSS Timer 1 one-shot (PLIC 82), loaded with
  `deadline - now`. Callout queue and latency code are STM32's unchanged. `HRT_TIMER_CLOCK` defaults to the APB clock
  (150 MHz); Renode's MPFS_Timer rate is unknown (model ships compiled), measure it in step 5.
- version: UUID = 128-bit DSN via `mpfs_read_dsn()`, read once. Renode has no system controller: the read returns
  zeros (unmapped reads are 0, so "not busy"), hence the fallback ID "chipwings-renode" on all-zero or timeout.
- board_critmon is empty on purpose: NuttX 12's critmon uses `perf_gettime()` (mpfs_perf.c); PX4 still links the target.
- No CONFIG_I2C/CONFIG_SPI yet, so the board has no i2c.cpp/spi.cpp (PX4's bus tables only exist with them).
- Params on tmpfs at /fs/microsd (`init/rc.board_early`); lost at reset.
- script.ld has explicit PHDRS: otherwise the writable `.init_array` makes the code segment RWX.
- NuttX patch 0002 goes into PX4's NuttX fork, 0001 must NOT: NuttX 12.12's riscv_mtimer adds no per-hart
  mtimecmp offset (13.1's adds `8 * up_cpu_index()`), so the fork's mpfs_timerisr.c is already right and 0001 makes
  hart 1 program hart 0's comparator (symptom: "ABC" then silence, hart 1 in up_idle). 0003 needs rebasing for step 6.

First boot, PX4 alone (step 5, done 2026-10-31): `sim/phase5/run.sh [gui|headless|stop]`, then
`sim/phase5/phase5_test.py [--target renode|board --serial DEV]` -> 13/13 (ver all, MSS timer calibration, tests hrt,
perf latency, top, work_queue, uorb top, param saved, mtime vs Renode virtual time 1,000,000/s).
- Renode's MPFS_Timer counts at 100 MHz (measured against mtime), not the 150 MHz APB clock. hrt.c measures the
  timer against mtime at init (1 ms) and logs `INFO [arch_hrt] MSS Timer 1 at N MHz`; same ELF on the board.
- perf latency: >= 99% of callouts in the 1 us bucket (Renode is not cycle accurate; real numbers at H3).
- PX4 patch 0004: `param load-or-init ''` failed on boards without storage (NuttX open("") is ENXIO, not ENOENT).
- `board_mcu_version` returns 0 with rev '?': -1 makes ver print "UNKNOWN MCU" instead of the part.
- Console buffer 32 KiB (`BOARD_CONSOLE_BUFFER_SIZE`): the 4 KiB default loses the start of boot from dmesg.
- `run.sh stop` quits through the monitor and waits; `kill` returns before Renode exits and a restart races the ports.
- Test harness: NSH prompts are no end marker (dmesg replays them) and an interrupted run leaves commands queued in
  the UART; phase5_test.py syncs on `echo __SYNC<random> $Q` and ends each command with `echo __DONE<n> $Q` (Q=42).
- Expected rcS noise until step 7: "command not found" for modules not built yet, no airframe for SYS_AUTOSTART 0.

RPMsg back on (step 6, done 2026-10-31): `sim/phase5/run.sh headless linux` (px4-linux.resc), then
`sim/phase4/rpmsg_test.py 2000` (2000/2000) and `sim/phase5/phase5_test.py --linux` (15/15: hartcheck, rpmsg_ping,
and every step 5 check with Linux running). Phase 4's `examples/run_examples.py` still 6/6 after the fixes below.
- Board `src/rpmsg.c` does what mpfs_bringup()+cwinit did: `mpfs_ihc_init()`, then `rptun_boot("mpfs-ihc")` (the fork
  names the CPU "mpfs-ihc", 13.1 "mpfsihc"). In C: libmetal's headers need C++ `<atomic>`, absent with -nostdinc++.
- rptun_boot() blocks until Linux sets DRIVER_OK (OpenAMP device role), so it runs in its own thread (`rpmsg_start`,
  priority 10, ~0.4% CPU while waiting). PX4 boots without Linux; `[rpmsg] Linux attached` appears when Linux does.
- PX4 patch 0005: PX4 never built or linked NuttX's openamp/ (no PX4 board used RPMsg). Builds it with CONFIG_OPENAMP,
  links it in the group and as an interface dependency of nuttx_drivers/nuttx_arch (and openamp -> drivers for
  up_addrenv_*), since those libraries are repeated after the group.
- OpenAMP/libmetal for the fork are pinned commits (c4683284 / a4bce350) plus NuttX's patches; fetched by hand with
  curl --retry and the .defs recipe (the .zip is the make target, so a bare download would never be unpacked).
- IHC lost-notification race (also in Phase 4, hidden by Renode's 100 us quantum; at 10 us 2 of 3 runs of 500 pings
  stalled): MP mirrors the remote's RMP and writing 0 consumes the message, but both NuttX (modifyreg32 ... ACK_INT)
  and our OpenSBI extension (ack clear, tx) did read-modify-writes that wrote back a stale MP=0, eating a message that
  arrived in between. Found with `logLevel 0 ihc`: "hart 2 consumed message from hart 1" with no ack after it. Fix:
  non-consuming control writes set MP (`MP_KEEP`): NuttX patch 0004 (sw/patches for 13.1, sw/px4-patches/nuttx for the
  fork), OpenSBI patch updated. After: 2000/2000 at 10 us. Real IP's MP write semantics unconfirmed (no spec found);
  writing 1 is harmless either way. Linux also knows an `microchip,miv-ihc-rtl-v2` IHC: check which one the 2025
  reference design has at H0 (the model is v1).
- px4-linux.resc sets the quantum to 10 us: at 100 us, HRT callouts ran up to 100 us late when Linux idled (22% of them);
  a hart sees an interrupt up to two quanta late, so phase5_test.py allows 5 us + 2 quanta in Renode with Linux.
  Open: 1 of 3483 callouts in the 101-1000 us bucket in one run; watch in step 7, measure at H3.
- Patch tracking: build-px4.sh and build-nuttx.sh record applied patches in sw/out/patch-stamps/ (a later patch can
  change an earlier one's context so neither --check nor --check --reverse works on it alone).
- The Linux image has no `timeout` and no `stress-ng`; step 7's load test needs stress-ng added to Buildroot.

Closed loop with SIH (step 7, PX4 alone done 2026-11-09; isolation checks still open): `sim/phase5/run.sh headless`, then
`commander arm`, `commander takeoff` on the NSH console (telnet localhost 33457).
- Board `default.px4board` now builds simulator_sih, commander, ekf2, sensors, mc_{att,pos,rate}_control, control_allocator,
  flight_mode_manager, navigator, land_detector, load_mon, mavlink, events, dataman, manual_control, rc_update (1.5 MB code).
  `rc.board_defaults` sets `SYS_AUTOSTART 1100` (1100_rc_quad_x_sih, SYS_HITL 2).
- Real bug found: modules hard-code stack sizes for 32-bit ARM (`sih` asks for 1560 bytes). On RV64 it used 100%, overflowed
  into the heap and NuttX asserted in `mm_mallinfo.c:72` (free list check) from `wq:lp_default`. PX4's POSIX layer already
  doubles stacks on 64-bit (`PX4_STACK_ADJUSTED`); the NuttX layer did not. PX4 patch 0006 scales `px4_task_spawn_cmd` stacks
  by `__SIZEOF_POINTER__ >> 2`. Work queues already use PX4_STACK_ADJUSTED.
- Result: EKF2 at 100 Hz (avg 1.25 ms per update, attitude/local/global position valid), arm + takeoff, z -2.50 m held,
  navigation mode Hold, no failsafe. Renode ran at ~1.0x real time. CPU 54% busy (rate_ctrl 16%, sih 16%, ekf2 14%,
  nav_and_controllers 6%). `perf latency`: 81180 of ~200k callouts in the 1 us bucket, 67473 in 50 us, 2238 in 100 us, none above.
- Expected noise: "command not found" for modules not built (tone_alarm, rgbled, px4io, rc_input, ...), UAVCAN_ENABLE and GPS_1_CONFIG
  not found, `param 65535 invalid` from parameters not present on this board.
- Isolation checks done 2026-11-09: `run.sh headless linux`, then `sim/phase5/isolation_test.py` -> 10/10. Hover at -2.5 m (+-0.01 m)
  while stress-ng (`--cpu 3 --vm 2`, 8 procs, loadavg 2.2) runs on the Linux harts; no failsafe, no callout over 100 us beyond
  baseline (3 vs 0 in the 100 us bucket, none above); then `echo c > /proc/sysrq-trigger` (Kernel panic) and PX4 keeps
  hovering, armed, in Hold, shell responsive. Linux image now has stress-ng (`BR2_PACKAGE_STRESS_NG`) and CONFIG_MAGIC_SYSRQ.
- Not covered: landing on failsafe when the MAVLink link drops (needs step 8, MAVLink over /dev/ttyRPMSG0), and real
  L2/DDR contention (Renode is not cycle accurate; repeat on the board at H4).
- Open: SIH check in phase5_test.py.
