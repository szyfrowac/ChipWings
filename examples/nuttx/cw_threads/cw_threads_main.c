/****************************************************************************
 * examples/nuttx/cw_threads/cw_threads_main.c
 *
 * cw_threads [messages_per_producer]
 *
 * Producer/consumer test of the NuttX primitives a flight stack is built
 * on: two producer threads send numbered messages through a POSIX message
 * queue, two consumer threads receive them and add them to a mutex-protected
 * total, and a counting semaphore tracks how many are in flight. Fails if a
 * message is lost or counted twice.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NPRODUCERS  2
#define NCONSUMERS  2
#define QUEUE_NAME  "/cw_threads"
#define STOP_MSG    UINT32_MAX

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_per_producer;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_sum;
static uint32_t g_received;
static sem_t g_in_flight;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR void *producer(FAR void *arg)
{
  int id = (int)(intptr_t)arg;
  mqd_t mq = mq_open(QUEUE_NAME, O_WRONLY);
  uint32_t value;
  int i;

  for (i = 0; i < g_per_producer; i++)
    {
      value = (uint32_t)(id * g_per_producer + i + 1);
      sem_post(&g_in_flight);
      mq_send(mq, (FAR const char *)&value, sizeof(value), 0);
    }

  mq_close(mq);
  return NULL;
}

static FAR void *consumer(FAR void *arg)
{
  mqd_t mq = mq_open(QUEUE_NAME, O_RDONLY);
  uint32_t value;

  for (; ; )
    {
      if (mq_receive(mq, (FAR char *)&value, sizeof(value), NULL) < 0)
        {
          continue;
        }

      if (value == STOP_MSG)
        {
          break;
        }

      sem_wait(&g_in_flight);
      pthread_mutex_lock(&g_lock);
      g_sum += value;
      g_received++;
      pthread_mutex_unlock(&g_lock);
    }

  mq_close(mq);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  pthread_t producers[NPRODUCERS];
  pthread_t consumers[NCONSUMERS];
  struct mq_attr attr;
  uint32_t stop = STOP_MSG;
  uint32_t total;
  uint64_t expected;
  int left;
  mqd_t mq;
  int i;

  g_per_producer = argc > 1 ? atoi(argv[1]) : 1000;
  if (g_per_producer <= 0)
    {
      printf("usage: cw_threads [messages_per_producer]\n");
      return EXIT_FAILURE;
    }

  g_sum = 0;
  g_received = 0;
  sem_init(&g_in_flight, 0, 0);

  attr.mq_maxmsg  = 8;
  attr.mq_msgsize = sizeof(uint32_t);
  attr.mq_flags   = 0;
  mq_unlink(QUEUE_NAME);
  mq = mq_open(QUEUE_NAME, O_CREAT | O_RDWR, 0666, &attr);
  if (mq == (mqd_t)-1)
    {
      printf("cw_threads: mq_open failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  for (i = 0; i < NCONSUMERS; i++)
    {
      pthread_create(&consumers[i], NULL, consumer, NULL);
    }

  for (i = 0; i < NPRODUCERS; i++)
    {
      pthread_create(&producers[i], NULL, producer, (FAR void *)(intptr_t)i);
    }

  for (i = 0; i < NPRODUCERS; i++)
    {
      pthread_join(producers[i], NULL);
    }

  for (i = 0; i < NCONSUMERS; i++)
    {
      mq_send(mq, (FAR const char *)&stop, sizeof(stop), 0);
    }

  for (i = 0; i < NCONSUMERS; i++)
    {
      pthread_join(consumers[i], NULL);
    }

  mq_close(mq);
  mq_unlink(QUEUE_NAME);
  sem_getvalue(&g_in_flight, &left);
  sem_destroy(&g_in_flight);

  /* Values 1 .. NPRODUCERS * per_producer, each exactly once */

  total    = (uint32_t)(NPRODUCERS * g_per_producer);
  expected = (uint64_t)total * (total + 1) / 2;

  printf("cw_threads: %d producers x %d messages, %d consumers: "
         "received %lu, sum %llu (expected %llu), in flight %d\n",
         NPRODUCERS, g_per_producer, NCONSUMERS, (unsigned long)g_received,
         (unsigned long long)g_sum, (unsigned long long)expected, left);

  if (g_received == total && g_sum == expected && left == 0)
    {
      printf("cw_threads: PASS\n");
      return EXIT_SUCCESS;
    }

  printf("cw_threads: FAIL\n");
  return EXIT_FAILURE;
}
