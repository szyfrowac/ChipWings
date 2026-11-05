/****************************************************************************
 * examples/common/cw_rpmsg_proto.h
 *
 * Messages exchanged between the NuttX examples (flight-control core, hart 1)
 * and the Linux examples (harts 2-4) over RPMsg. Both sides are RV64
 * little-endian and build this header unchanged.
 *
 * Endpoints (NuttX creates them, the name service announces them to Linux):
 *   "cw-echo"  every message is sent back unchanged to its sender
 *   "cw-imu"   simulated IMU stream: Linux sends CW_IMU_START, NuttX answers
 *              with CW_IMU_SAMPLE messages at the requested rate, then one
 *              CW_IMU_DONE
 *
 ****************************************************************************/

#ifndef __EXAMPLES_COMMON_CW_RPMSG_PROTO_H
#define __EXAMPLES_COMMON_CW_RPMSG_PROTO_H

#include <stdint.h>

#define CW_ECHO_EPT_NAME   "cw-echo"
#define CW_IMU_EPT_NAME    "cw-imu"

/* Largest payload of one RPMsg message with 512-byte buffers (16-byte
 * RPMsg header)
 */

#define CW_RPMSG_MAX_PAYLOAD  496

enum cw_imu_type_e
{
  CW_IMU_START  = 1,   /* Linux -> NuttX: struct cw_imu_start_s */
  CW_IMU_STOP   = 2,   /* Linux -> NuttX: struct cw_imu_hdr_s */
  CW_IMU_SAMPLE = 3,   /* NuttX -> Linux: struct cw_imu_sample_s */
  CW_IMU_DONE   = 4,   /* NuttX -> Linux: struct cw_imu_done_s */
};

struct cw_imu_hdr_s
{
  uint32_t type;
} __attribute__((packed));

struct cw_imu_start_s
{
  uint32_t type;       /* CW_IMU_START */
  uint32_t rate_hz;    /* samples per second, 1..1000 */
  uint32_t count;      /* samples to send */
} __attribute__((packed));

struct cw_imu_sample_s
{
  uint32_t type;       /* CW_IMU_SAMPLE */
  uint32_t seq;        /* 0, 1, 2, ... */
  uint64_t t_ns;       /* NuttX CLOCK_MONOTONIC when the sample was taken */
  float    accel[3];   /* m/s^2 */
  float    gyro[3];    /* rad/s */
} __attribute__((packed));

struct cw_imu_done_s
{
  uint32_t type;       /* CW_IMU_DONE */
  uint32_t sent;       /* samples sent */
  uint32_t late;       /* periods where NuttX woke up more than one period late */
} __attribute__((packed));

#endif /* __EXAMPLES_COMMON_CW_RPMSG_PROTO_H */
