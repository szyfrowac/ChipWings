# Chipwings examples

Small test programs for the two sides of the PolarFire SoC AMP system and the link between them:
NuttX on the flight-control core (U54 hart 1) and Linux on the mission-computer cores (U54 harts 2-4),
talking over RPMsg (OpenAMP on NuttX, virtio_rpmsg_bus on Linux) with notifications through the Mi-V IHC.

Every program ends with a line `<name>: PASS` or `<name>: FAIL`.

| Program | Runs on | Tests |
|---|---|---|
| `cw_timer [iterations] [period_ms]` | NuttX (NSH) | Periodic wake-ups with `clock_nanosleep(TIMER_ABSTIME)`, like a control loop: no early wake-up, at most 2 ticks late, no drift. Uses the per-hart CLINT timer fixed by `sw/patches/0001`. |
| `cw_threads [messages]` | NuttX (NSH) | 2 producer and 2 consumer pthreads over a POSIX message queue, with a mutex-protected total and a counting semaphore: nothing lost or counted twice. |
| `cw_rpmsg_echo` | NuttX (NSH) | Creates the OpenAMP endpoint `cw-echo`, which sends every message back to its sender. Runs in the background after the first call; a second call prints its counters. |
| `cw_rpmsg_imu &` | NuttX (NSH) | Creates the endpoint `cw-imu`: on request streams simulated IMU samples (sequence number, NuttX timestamp, accel, gyro) at a fixed rate from a `clock_nanosleep` loop. |
| `hartcheck` | Linux | Linux uses exactly harts 2, 3, 4 and never hart 1: CPU-to-hart mapping from `/proc/cpuinfo`, plus a pinned thread on every CPU. |
| `rpmsg_ping [rounds] [bytes]` | Linux | Round trips to NuttX's built-in `rpmsg-ping` endpoint (no NuttX app needed); NuttX checks the payload, Linux checks the answer; latency. |
| `rpmsg_echo [rounds]` | Linux | Every message size from 1 to 496 bytes (the largest RPMsg payload) to `cw-echo` and back, byte for byte; latency and throughput. Needs `cw_rpmsg_echo` on NuttX. |
| `rpmsg_imu [rate_hz] [count]` | Linux | Requests a stream from `cw-imu` and checks every sample arrives once and in order, that NuttX's timestamps are spaced at the requested rate (within 5 %), and that the data looks like a level IMU. Needs `cw_rpmsg_imu &` on NuttX. |

## Layout

```
examples/
  common/cw_rpmsg_proto.h   message formats and endpoint names shared by both sides
  nuttx/                    NuttX apps (NSH commands), built into the NuttX image
  linux/                    Linux programs, built into the root filesystem (/usr/bin)
  run_examples.py           runs all of them in the simulation and reports PASS/FAIL
```

## Build

NuttX: `sw/build-nuttx.sh` links `examples/nuttx` into the NuttX apps tree as `apps/cwexamples`.
`cw_timer` and `cw_threads` are enabled in all three configs (`nsh-hart1`, `nsh-amp-hart1`, `nsh-rpmsg-hart1`);
the two RPMsg apps need `nsh-rpmsg-hart1` (they depend on `CONFIG_RPTUN`).

```sh
sw/build-nuttx.sh nsh-rpmsg-hart1
```

Linux: the Buildroot package `sw/br2-chipwings/package/chipwings-examples` builds `examples/linux` with the
target toolchain and installs it to `/usr/bin` (enabled in `chipwings_mpfs_linux_defconfig`). After changing
the sources:

```sh
sw/build-linux.sh chipwings-examples-rebuild all     # 'all' repacks the initramfs into the kernel Image
```

Quick compile check on the host, without touching the image:

```sh
make -C examples/linux CC=$PWD/sw/br2-out/host/bin/riscv64-linux-gcc
make -C examples/linux clean
```

## Run

```sh
sim/phase4/run.sh headless          # NSH on telnet 33457, Linux on telnet 33458 (root/root)
examples/run_examples.py            # all examples, PASS/FAIL summary
```

By hand, in NSH:

```
nsh> cw_timer 100 10
nsh> cw_threads 1000
nsh> cw_rpmsg_echo
nsh> cw_rpmsg_imu &
```

then in Linux:

```
# hartcheck
# rpmsg_ping 200 64
# rpmsg_echo
# rpmsg_imu 100 500
```

## How a Linux program reaches a NuttX endpoint

When a NuttX app calls `rpmsg_create_ept(..., "cw-echo", RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, ...)`, OpenAMP sends a
name-service announcement. Linux creates `/sys/bus/rpmsg/devices/virtio0.cw-echo.-1.<addr>`, whose `dst` is the
NuttX endpoint address. `cw_rpmsg_open()` (`linux/cw_rpmsg_util.c`) reads that address, creates a local endpoint
for it with `RPMSG_CREATE_EPT_IOCTL` on `/dev/rpmsg_ctrl0` (module `rpmsg_char`, loaded on demand) and opens the
resulting `/dev/rpmsgN`: `write()` sends one message, `read()` returns one. NuttX answers to the sender's address
(`rpmsg_sendto(ept, ..., src)`), so several Linux programs can use one NuttX endpoint at the same time.
