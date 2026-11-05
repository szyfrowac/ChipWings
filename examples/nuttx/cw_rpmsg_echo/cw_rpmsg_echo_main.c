/****************************************************************************
 * examples/nuttx/cw_rpmsg_echo/cw_rpmsg_echo_main.c
 *
 * cw_rpmsg_echo
 *
 * Creates the OpenAMP endpoint "cw-echo" on the RPMsg link to Linux. The
 * name service announces it, Linux creates a channel for it, and every
 * message a Linux program sends to it comes straight back to the sender
 * (examples/linux/rpmsg_echo).
 *
 * The first run registers the endpoint and returns; it keeps working in the
 * background (the callback runs in the RPMsg thread). Later runs print how
 * many messages it has echoed.
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

#include <nuttx/rpmsg/rpmsg.h>

#include "cw_rpmsg_proto.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rpmsg_endpoint g_ept;
static volatile uint32_t g_messages;
static volatile uint64_t g_bytes;
static volatile uint32_t g_errors;
static bool g_registered;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int echo_cb(FAR struct rpmsg_endpoint *ept, FAR void *data,
                   size_t len, uint32_t src, FAR void *priv)
{
  /* Reply to whoever sent it: one Linux program per endpoint address */

  if (rpmsg_sendto(ept, data, len, src) < 0)
    {
      g_errors++;
    }

  g_messages++;
  g_bytes += len;
  return 0;
}

static void device_created(FAR struct rpmsg_device *rdev, FAR void *priv)
{
  int ret;

  if (g_ept.rdev != NULL)
    {
      return;
    }

  ret = rpmsg_create_ept(&g_ept, rdev, CW_ECHO_EPT_NAME,
                         RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, echo_cb, NULL);
  if (ret < 0)
    {
      printf("cw_rpmsg_echo: rpmsg_create_ept failed: %d\n", ret);
      return;
    }

  printf("cw_rpmsg_echo: endpoint \"%s\" at 0x%" PRIx32 " on link to %s\n",
         CW_ECHO_EPT_NAME, g_ept.addr, rpmsg_get_cpuname(rdev));
}

static void device_destroy(FAR struct rpmsg_device *rdev, FAR void *priv)
{
  if (g_ept.rdev == rdev)
    {
      rpmsg_destroy_ept(&g_ept);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  if (g_registered)
    {
      printf("cw_rpmsg_echo: running, echoed %lu messages (%llu bytes), "
             "%lu send errors\n", (unsigned long)g_messages,
             (unsigned long long)g_bytes, (unsigned long)g_errors);
      return EXIT_SUCCESS;
    }

  ret = rpmsg_register_callback(NULL, device_created, device_destroy,
                                NULL, NULL);
  if (ret < 0)
    {
      printf("cw_rpmsg_echo: rpmsg_register_callback failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  g_registered = true;
  if (g_ept.rdev == NULL)
    {
      printf("cw_rpmsg_echo: waiting for the RPMsg link to Linux\n");
    }

  return EXIT_SUCCESS;
}
