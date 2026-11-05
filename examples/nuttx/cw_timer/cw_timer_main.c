/****************************************************************************
 * examples/nuttx/cw_timer/cw_timer_main.c
 *
 * cw_timer [iterations] [period_ms]
 *
 * Periodic task on the flight-control core: wakes up every period_ms with
 * clock_nanosleep(TIMER_ABSTIME), the way a control loop would, and reports
 * how late each wake-up was. Fails if a wake-up comes early or more than two
 * system ticks late, or if the loop drifts.
 *
 * This exercises the per-hart CLINT timer that sw/patches/0001 fixed: with
 * the original mpfs_timerisr.c NuttX never got a tick and this hangs.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef NSEC_PER_SEC
#  define NSEC_PER_SEC 1000000000LL
#endif
#define TICK_NS       ((int64_t)CONFIG_USEC_PER_TICK * 1000)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int64_t ts_ns(FAR const struct timespec *ts)
{
  return (int64_t)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

static void ns_ts(int64_t ns, FAR struct timespec *ts)
{
  ts->tv_sec  = ns / NSEC_PER_SEC;
  ts->tv_nsec = ns % NSEC_PER_SEC;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int iterations = argc > 1 ? atoi(argv[1]) : 200;
  int period_ms  = argc > 2 ? atoi(argv[2]) : 10;
  int64_t period = (int64_t)period_ms * 1000000;
  int64_t start;
  int64_t next;
  int64_t late;
  int64_t min_late = INT64_MAX;
  int64_t max_late = INT64_MIN;
  int64_t sum_late = 0;
  int64_t elapsed;
  int early = 0;
  struct timespec ts;
  int i;

  if (iterations <= 0 || period_ms <= 0)
    {
      printf("usage: cw_timer [iterations] [period_ms]\n");
      return EXIT_FAILURE;
    }

  printf("cw_timer: %d wake-ups every %d ms (system tick %d us)\n",
         iterations, period_ms, CONFIG_USEC_PER_TICK);

  clock_gettime(CLOCK_MONOTONIC, &ts);
  start = ts_ns(&ts);
  next  = start;

  for (i = 0; i < iterations; i++)
    {
      next += period;
      ns_ts(next, &ts);
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
      clock_gettime(CLOCK_MONOTONIC, &ts);

      late = ts_ns(&ts) - next;
      if (late < 0)
        {
          early++;
        }

      min_late  = late < min_late ? late : min_late;
      max_late  = late > max_late ? late : max_late;
      sum_late += late;
    }

  elapsed = ts_ns(&ts) - start;

  printf("cw_timer: lateness min %lld us, avg %lld us, max %lld us, "
         "early wake-ups %d\n",
         (long long)(min_late / 1000),
         (long long)(sum_late / iterations / 1000),
         (long long)(max_late / 1000), early);
  printf("cw_timer: %d periods took %lld ms (expected %lld ms)\n",
         iterations, (long long)(elapsed / 1000000),
         (long long)(period * iterations / 1000000));

  if (early == 0 && max_late <= 2 * TICK_NS &&
      elapsed - period * iterations <= 2 * TICK_NS)
    {
      printf("cw_timer: PASS\n");
      return EXIT_SUCCESS;
    }

  printf("cw_timer: FAIL\n");
  return EXIT_FAILURE;
}
