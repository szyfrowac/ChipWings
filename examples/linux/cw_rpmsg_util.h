/*
 * examples/linux/cw_rpmsg_util.h
 *
 * Talk to a NuttX RPMsg endpoint from Linux user space with the stock
 * rpmsg_char driver: find the channel NuttX announced by name, create a
 * local endpoint for it through /dev/rpmsg_ctrl0, and open its /dev/rpmsgN.
 */

#ifndef CW_RPMSG_UTIL_H
#define CW_RPMSG_UTIL_H

#include <stdint.h>
#include <time.h>

/* Open an endpoint talking to the NuttX endpoint called name.
 * Returns the /dev/rpmsgN file descriptor, or -1 with a message on stderr.
 */
int cw_rpmsg_open(const char *name);

/* Destroy the endpoint and close it */
void cw_rpmsg_close(int fd);

/* Wait up to timeout_ms for a message; returns its length, 0 on timeout */
int cw_rpmsg_recv(int fd, void *buf, int len, int timeout_ms);

static inline int64_t cw_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#endif
