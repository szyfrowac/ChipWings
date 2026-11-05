# ChipWings — PolarFire SoC Heterogeneous AMP UAV Avionics Platform

**ChipWings** is a single-chip UAV avionics platform targeting the **Microchip PolarFire SoC (MPFS250T / Icicle Kit)** in an **Asymmetric Multiprocessing (AMP)** configuration, validated end-to-end in **Renode**:

- **U54 Hart 1 (Real-Time Flight Control):** Runs **Apache NuttX RTOS** / **PX4 Autopilot** (`chipwings_mpfs-hart1_default`) with deterministic scheduling, CLINT `mtime` + MSS Timer 1 High-Resolution Timer (`hrt`), and zero OS jitter from the companion computer.
- **U54 Harts 2–4 (Mission Computer):** Runs **3-core SMP Linux** (`linux4microchip+fpga-2025.10`, kernel 6.12.48 via Buildroot 2026.02.3 LTS) with `miv-remoteproc` and `virtio_rpmsg_bus`.
- **Inter-Domain Link:** Hardware-assisted **Mi-V Inter-Hart Communication (IHC)** + **OpenAMP / RPMsg** over shared non-cached DDR (`0xA2300000` resource table, `0xA2400000` vrings/buffers).

---

## Repository Structure

```text
ChipWings/
├── examples/                # NuttX & Linux AMP/RPMsg test suite + automated runner
│   ├── common/              # Shared RPMsg wire protocol (cw_rpmsg_proto.h)
│   ├── linux/               # Linux userspace tests (hartcheck, rpmsg_ping, rpmsg_echo, rpmsg_imu)
│   ├── nuttx/               # NuttX NSH apps (cw_timer, cw_threads, cw_rpmsg_echo, cw_rpmsg_imu)
│   └── run_examples.py      # End-to-end automated test runner (6/6 checks)
├── sim/                     # Renode platform, custom C# peripherals, and phased simulation scripts
│   ├── peripherals/         # MiV_IHC.cs, SafeIrqRiscV64.cs, SafeIpiCoreLevelInterruptor.cs
│   ├── platforms/           # chipwings-mpfs.repl (PolarFire SoC + thread-safe IPI/IRQ + Mi-V IHC)
│   ├── phase0/ .. phase5/   # Phased Renode scripts (.resc), launchers (run.sh), and test harnesses
│   └── README.md            # Detailed engineering log & phase notes
├── sw/                      # Build scripts, board ports, configs, and upstream patches
│   ├── br2-chipwings/       # Buildroot external tree (Linux DT, defconfig, OpenSBI IHC extension)
│   ├── chipwings-apps/      # Custom NuttX apps (hartinfo, cwinit)
│   ├── configs/             # NuttX defconfigs & linker scripts (nsh-hart1, nsh-amp-hart1, nsh-rpmsg-hart1)
│   ├── patches/             # NuttX 13.1 patches (CLINT mtimecmp offset, fixed rsc_table, IHC fixes)
│   ├── px4-chipwings/       # Out-of-tree PX4 port (platforms/nuttx/src/px4/microchip + boards/chipwings/mpfs-hart1)
│   ├── px4-patches/         # Patches for PX4 main & PX4's NuttX fork
│   ├── build-nuttx.sh       # Builds NuttX for U54_1
│   ├── build-linux.sh       # Builds OpenSBI + Linux + rootfs + DTB via Buildroot for U54_2..4
│   └── build-px4.sh         # Builds PX4 Autopilot for U54_1
├── TUTORIAL_GUIDE.md        # Architecture & porting walkthrough
└── px4-mpfs-plan.html       # PX4 MPFS port & Icicle Kit hardware validation plan (H0–H6)
```

---

## 1. Prerequisites & Setup

### Host Tools

1. **Renode 1.17.0 (portable):**
   Install at `~/tools/renode_1.17.0-portable/` (or override `RENODE=/path/to/renode`):
   ```bash
   mkdir -p ~/tools
   curl -L --retry 5 https://dl.antmicro.com/projects/renode/builds/renode-1.17.0.linux-portable.tar.gz | tar xz -C ~/tools
   mv ~/tools/renode_1.17.0_portable ~/tools/renode_1.17.0-portable
   ```

2. **xPack RISC-V Bare-Metal GCC (`riscv-none-elf-gcc` 15.2.0-1):**
   Install at `~/tools/xpack-riscv-none-elf-gcc-15.2.0-1/` (or override `TOOLCHAIN=/path/to/bin`):
   ```bash
   curl -L --retry 5 https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1-linux-x64.tar.gz | tar xz -C ~/tools
   ```

3. **Standard Host Packages:**
   ```bash
   sudo apt-get install -y build-essential cmake ninja-build git kconfig-frontends \
     python3 python3-pip netcat-openbsd telnet curl rsync unzip bc cpio
   ```

### Fetching External Source Trees into `sw/`

All ChipWings modifications live in tracked overlay/patch directories (`sw/patches/`, `sw/px4-chipwings/`, `sw/px4-patches/`, `sw/br2-chipwings/`) and are applied automatically by the build scripts. Clone the upstream trees into `sw/`:

```bash
cd sw

# 1. Apache NuttX 13.1.0 & nuttx-apps (for Phases 1, 3, 4)
git clone --branch nuttx-13.1.0 --depth 1 https://github.com/apache/nuttx.git nuttx
git clone --branch nuttx-13.1.0 --depth 1 https://github.com/apache/nuttx-apps.git apps

# 2. Buildroot 2026.02.3 LTS (for Phases 2, 3, 4, 5 Linux side)
curl -L --retry 5 https://buildroot.org/downloads/buildroot-2026.02.3.tar.xz | tar xJ
mv buildroot-2026.02.3 buildroot

# 3. PX4-Autopilot (for Phase 5)
git clone https://github.com/PX4/PX4-Autopilot.git px4
git -C px4 checkout 5f429033bfc8cbd94143efe4e99a1acc8ae7d5bf
git -C px4 submodule update --init --recursive \
  platforms/nuttx/NuttX/nuttx \
  platforms/nuttx/NuttX/apps \
  src/modules/mavlink/mavlink \
  src/lib/events/libevents

# 4. Python virtualenv for PX4 code generation
python3 -m venv px4-venv
./px4-venv/bin/pip install pyros-genmsg empy==3.3.4 kconfiglib jinja2 pyyaml pymavlink nunavut jsonschema

cd ..
```

> **Note on OpenAMP / libmetal (for Phase 4 & Phase 5 RPMsg):**
> NuttX downloads `open-amp` and `libmetal` on first build when `CONFIG_OPENAMP=y` is enabled. If `make distclean` is used later, initializing git inside `sw/nuttx/openamp/{open-amp,libmetal}` preserves the unpacked archives across clean builds.

---

## 2. Building the Firmware & Linux Images

### Build NuttX (Phases 1, 3, 4)

`sw/build-nuttx.sh` applies `sw/patches/*.patch` once, links `sw/chipwings-apps` and `examples/nuttx` into `sw/apps/`, configures the target, and outputs the ELF/binary to `sw/out/<config>/`:

```bash
# Phase 1: Standalone NuttX on U54 Hart 1 (linked at 0x80000000)
./sw/build-nuttx.sh nsh-hart1

# Phase 3: AMP NuttX on U54 Hart 1 (linked in reserved DDR at 0xA2000000)
./sw/build-nuttx.sh nsh-amp-hart1

# Phase 4: AMP + OpenAMP/RPMsg over Mi-V IHC (resource table at 0xA2300000)
./sw/build-nuttx.sh nsh-rpmsg-hart1
```

### Build Linux (Phases 2, 3, 4, 5)

`sw/build-linux.sh` builds OpenSBI 1.7 (`fw_jump.elf` with the Microchip IHC vendor SBI extension `0x09000029`), Linux 6.12.48 (`Image` with built-in BusyBox initramfs + `examples/linux`), and `chipwings-mpfs-linux.dtb` into `sw/br2-out/images/`:

```bash
# First-time configure and full build (~5–20 min depending on core count)
./sw/build-linux.sh

# Rebuild only the ChipWings Linux example binaries and repack the kernel image
./sw/build-linux.sh chipwings-examples-rebuild all

# Rebuild kernel + DTB after editing chipwings-mpfs-linux.dts
./sw/build-linux.sh linux-rebuild all
```

### Build PX4 Autopilot (Phase 5)

`sw/build-px4.sh` creates the `riscv64-unknown-elf-*` toolchain wrapper shim, overlays `sw/px4-chipwings/` onto `sw/px4/`, applies `sw/px4-patches/`, builds `chipwings_mpfs-hart1_default`, and copies the ELF to `sw/out/px4/chipwings_mpfs-hart1_default.elf`:

```bash
# Verify toolchain shim and Python venv
./sw/build-px4.sh --toolchain

# Build PX4 for U54 Hart 1
./sw/build-px4.sh
```

---

## 3. Running & Testing Each Phase in Renode

Every phase provides a `run.sh` launcher supporting both interactive GUI mode and headless mode (`telnet` consoles).

| Port | Service | Credentials / Usage |
|---|---|---|
| `localhost:33456` | Renode Monitor | `telnet localhost 33456` |
| `localhost:33457` | U54 Hart 1 Console (`MMUART1` — NuttX / PX4 NSH) | `telnet localhost 33457` |
| `localhost:33458` | U54 Harts 2–4 Console (`MMUART3` — Linux) | `telnet localhost 33458` (login: `root` / `root`) |

---

### Phase 1 — NuttX Standalone on U54 Hart 1

```bash
./sw/build-nuttx.sh nsh-hart1
./sim/phase1/run.sh headless
```

Verify single-core execution and hardware hart assignment:
```bash
# From another terminal on the host:
./sim/phase1/check-one-core.sh
# Shows only u54_1 executing instructions; e51 and u54_2..4 halted at 0 instructions.

# Connect to NSH (telnet localhost 33457):
nsh> hartinfo
nsh> ostest
```

---

### Phase 2 — 3-Core SMP Linux on U54 Harts 2–4

```bash
./sw/build-linux.sh
./sim/phase2/run.sh headless
```

Connect via `telnet localhost 33458` (login `root` / `root`) and verify:
```bash
# nproc                 # -> 3
# cat /proc/cpuinfo     # -> harts 2, 3, 4
# grep Reserved /proc/iomem
#   a2000000-a22fffff : Reserved (NuttX)
#   a2300000-a2300fff : Reserved (rsc-table)
```

---

### Phase 3 — AMP: NuttX (Hart 1) + Linux SMP (Harts 2–4) Concurrently

```bash
./sw/build-nuttx.sh nsh-amp-hart1
./sim/phase3/run.sh headless
./sim/phase1/check-one-core.sh
```

Both `MMUART1` (NuttX NSH on port `33457`) and `MMUART3` (Linux on port `33458`) boot simultaneously in ~2 seconds without timer or IPI interference.

---

### Phase 4 — OpenAMP / RPMsg over Mi-V IHC + Example Suite

```bash
./sw/build-nuttx.sh nsh-rpmsg-hart1
./sim/phase4/run.sh headless
```

Run the automated 500-round-trip RPMsg ping test and the full 6-program example test suite (`cw_timer`, `cw_threads`, `hartcheck`, `rpmsg_ping`, `rpmsg_echo`, `rpmsg_imu`):

```bash
# 500 checked round trips over /dev/rpmsg0
./sim/phase4/rpmsg_test.py 500

# Full NuttX + Linux AMP example test suite (expects 6/6 PASS)
./examples/run_examples.py
```

---

### Phase 5 — PX4 Autopilot on U54 Hart 1 + Linux Companion Computer

#### A. PX4 Standalone on U54 Hart 1 (13/13 automated checks)

```bash
./sw/build-px4.sh
./sim/phase5/run.sh headless
./sim/phase5/phase5_test.py
./sim/phase5/run.sh stop
```

`phase5_test.py` validates:
- `ver all` (`CHIPWINGS_MPFS_HART1`, RV64, UUID fallback `chipwings-renode`)
- MSS Timer 1 runtime calibration against 1 MHz CLINT `mtime`
- `tests hrt` (High-Resolution Timer deadlines & callout queue)
- `perf latency` histogram
- `top`, `work_queue status`, `uorb top -1`, `param status`
- `mtime` tick rate accuracy against Renode virtual time (`1,000,000 ticks/s`)

#### B. PX4 + Linux SMP + RPMsg (15/15 automated checks + 2000 RPMsg pings)

```bash
./sim/phase5/run.sh headless linux
./sim/phase4/rpmsg_test.py 2000
./sim/phase5/phase5_test.py --linux
./sim/phase5/run.sh stop
```

---

## Further Documentation

- [`sim/README.md`](sim/README.md) — Detailed technical notes, hardware/Renode bugs discovered and fixed (NuttX `mpfs_timerisr` double offset, Linux CLINT slot shift, IHC `MP_KEEP` control register race, Renode multi-hart `mip` race), and memory map.
- [`examples/README.md`](examples/README.md) — Documentation for the NuttX & Linux RPMsg test applications.
- [`TUTORIAL_GUIDE.md`](TUTORIAL_GUIDE.md) — Step-by-step architecture and porting guide.
- [`px4-mpfs-plan.html`](px4-mpfs-plan.html) — PX4 port architecture & PolarFire SoC Icicle Kit hardware validation ladder (`H0`–`H6`).
