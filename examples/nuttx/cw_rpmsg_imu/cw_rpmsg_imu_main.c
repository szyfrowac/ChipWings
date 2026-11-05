/****************************************************************************
 * examples/nuttx/cw_rpmsg_imu/cw_rpmsg_imu_main.c
 *
 * cw_rpmsg_imu &
 *
 * Flight-control-to-mission-computer data path: creates the OpenAMP endpoint
 * "cw-imu" and, when a Linux program sends CW_IMU_START (rate, count), streams
 * that many simulated IMU samples to it at that rate, then CW_IMU_DONE
 * (examples/linux/rpmsg_imu). Samples are timed with clock_nanosleep
 * (TIMER_ABSTIME) like a real sensor loop and carry the NuttX timestamp and a
 * sequence number, so Linux can check rate, loss and ordering.
 *
 * Run it in the background; it serves one stream at a time and stays up.
 * Running it again while it is up prints its counters.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <time.h>

#include <nuttx/rpmsg/rpmsg.h>

#include "cw_rpmsg_proto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef NSEC_PER_SEC
#  define NSEC_PER_SEC 1000000000LL
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rpmsg_endpoint g_ept;
static sem_t g_start = SEM_INITIALIZER(0);
static volatile bool g_running;
static volatile bool g_stop;
static volatile uint32_t g_rate_hz;
static volatile uint32_t g_count;
static volatile uint32_t g_dest;
static volatile uint32_t g_streams;
static volatile uint32_t g_samples;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int64_t now_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

/* Deterministic sensor noise, +-1.0 */

static float noise(FAR uint32_t *state)
{
  *state = *state * 1664525u + 1013904223u;
  return (float)(int32_t)(*state >> 8 & 0xffff) / 32768.0f - 1.0f;
}

static int imu_cb(FAR struct rpmsg_endpoint *ept, FAR void *data,
                  size_t len, uint32_t src, FAR void *priv)
{
  FAR const struct cw_imu_start_s *start = data;

  if (len < sizeof(struct cw_imu_hdr_s))
    {
      return 0;
    }

  if (start->type == CW_IMU_STOP)
    {
      g_stop = true;
    }
  else if (start->type == CW_IMU_START && len >= sizeof(*start) &&
           !g_running && start->rate_hz > 0 && start->rate_hz <= 1000)
    {
      g_rate_hz = start->rate_hz;
      g_count   = start->count;
      g_dest    = src;
      g_stop    = false;
      g_running = true;
      sem_post(&g_start);
    }

  return 0;
}

static void device_created(FAR struct rpmsg_device *rdev, FAR void *priv)
{
  if (g_ept.rdev == NULL &&
      rpmsg_create_ept(&g_ept, rdev, CW_IMU_EPT_NAME, RPMSG_ADDR_ANY,
                       RPMSG_ADDR_ANY, imu_cb, NULL) == 0)
    {
      printf("cw_rpmsg_imu: endpoint \"%s\" at 0x%" PRIx32 " on link to "
             "%s\n", CW_IMU_EPT_NAME, g_ept.addr, rpmsg_get_cpuname(rdev));
    }
}

static void device_destroy(FAR struct rpmsg_device *rdev, FAR void *priv)
{
  if (g_ept.rdev == rdev)
    {
      rpmsg_destroy_ept(&g_ept);
    }
}

static void stream(void)
{
  struct cw_imu_sample_s sample;
  struct cw_imu_done_s done;
  struct timespec ts;
  int64_t period = NSEC_PER_SEC / g_rate_hz;
  int64_t next = now_ns();
  int64_t t;
  uint32_t rng = 1;
  uint32_t late = 0;
  uint32_t seq;

  memset(&sample, 0, sizeof(sample));
  sample.type = CW_IMU_SAMPLE;

  for (seq = 0; seq < g_count && !g_stop; seq++)
    {
      next += period;
      ts.tv_sec  = next / NSEC_PER_SEC;
      ts.tv_nsec = next % NSEC_PER_SEC;
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

      t = now_ns();
      if (t - next > period)
        {
          late++;
        }

      /* Level, nearly still airframe: gravity on z plus small noise */

      sample.seq      = seq;
      sample.t_ns     = (uint64_t)t;
      sample.accel[0] = 0.05f * noise(&rng);
      sample.accel[1] = 0.05f * noise(&rng);
      sample.accel[2] = 9.81f + 0.05f * noise(&rng);
      sample.gyro[0]  = 0.002f * noise(&rng);
      sample.gyro[1]  = 0.002f * noise(&rng);
      sample.gyro[2]  = 0.002f * noise(&rng);

      if (rpmsg_sendto(&g_ept, &sample, sizeof(sample), g_dest) < 0)
        {
          break;
        }

      g_samples++;
    }

  done.type = CW_IMU_DONE;
  done.sent = seq;
  done.late = late;
  rpmsg_sendto(&g_ept, &done, sizeof(done), g_dest);
  g_streams++;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  if (g_ept.rdev != NULL)
    {
      printf("cw_rpmsg_imu: running, %lu streams, %lu samples sent\n",
             (unsigned long)g_streams, (unsigned long)g_samples);
      return EXIT_SUCCESS;
    }

  ret = rpmsg_register_callback(NULL, device_created, device_destroy,
                                NULL, NULL);
  if (ret < 0)
    {
      printf("cw_rpmsg_imu: rpmsg_register_callback failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  for (; ; )
    {
      sem_wait(&g_start);
      stream();
      g_running = false;
    }

  return EXIT_SUCCESS;
}
