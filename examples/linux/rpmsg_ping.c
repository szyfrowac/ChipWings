/*
 * examples/linux/rpmsg_ping.c
 *
 * rpmsg_ping [rounds] [payload_bytes]
 *
 * Round trips to the "rpmsg-ping" endpoint that NuttX's RPMsg stack creates
 * by itself (drivers/rpmsg/rpmsg_ping.c, CONFIG_RPMSG_PING): no NuttX
 * application needed. Each request asks NuttX to check the payload and
 * answer; the answer's payload is checked here too. Prints latency.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cw_rpmsg_proto.h"
#include "cw_rpmsg_util.h"

/* NuttX struct rpmsg_ping_msg_s */
struct ping_msg {
	uint32_t cmd;
	uint32_t len;
	uint64_t cookie;
	uint8_t data[];
} __attribute__((packed));

#define PING_ACK    0x1	/* please answer */
#define PING_CHECK  0x2	/* please check that all data bytes are equal */
#define PING_RSP    0x20	/* this is the answer */

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 100;
	int payload = argc > 2 ? atoi(argv[2]) : 64;
	int len = (int)sizeof(struct ping_msg) + payload;
	uint8_t tx[CW_RPMSG_MAX_PAYLOAD], rx[CW_RPMSG_MAX_PAYLOAD];
	struct ping_msg *req = (struct ping_msg *)tx, *rsp = (struct ping_msg *)rx;
	int64_t t, lat, min = INT64_MAX, max = 0, sum = 0;
	int i, n, ok = 0, fd;

	if (rounds <= 0 || payload < 1 || len > CW_RPMSG_MAX_PAYLOAD) {
		fprintf(stderr, "usage: rpmsg_ping [rounds] [payload 1..%d]\n",
			CW_RPMSG_MAX_PAYLOAD - (int)sizeof(struct ping_msg));
		return 1;
	}
	fd = cw_rpmsg_open("rpmsg-ping");
	if (fd < 0) {
		printf("%s: FAIL\n", "rpmsg_ping");
		return 1;
	}

	for (i = 0; i < rounds; i++) {
		req->cmd = PING_ACK | PING_CHECK;
		req->len = (uint32_t)len;
		req->cookie = 0;
		memset(req->data, 'A' + i % 26, (size_t)payload);

		t = cw_now_ns();
		if (write(fd, tx, (size_t)len) != len) {
			perror("write");
			break;
		}
		n = cw_rpmsg_recv(fd, rx, sizeof(rx), 2000);
		lat = cw_now_ns() - t;
		if (n <= 0) {
			printf("rpmsg_ping: round %d: no answer within 2 s\n", i);
			break;
		}
		if (n != len || rsp->cmd != PING_RSP || memcmp(rsp->data, req->data, (size_t)payload)) {
			printf("rpmsg_ping: round %d: bad answer (%d bytes, cmd 0x%x)\n", i, n, rsp->cmd);
			continue;
		}
		ok++;
		sum += lat;
		min = lat < min ? lat : min;
		max = lat > max ? lat : max;
	}
	cw_rpmsg_close(fd);

	if (ok)
		printf("rpmsg_ping: %d/%d round trips of %d bytes, latency min %lld us, "
		       "avg %lld us, max %lld us\n", ok, rounds, len, (long long)(min / 1000),
		       (long long)(sum / ok / 1000), (long long)(max / 1000));
	printf("rpmsg_ping: %s\n", ok == rounds ? "PASS" : "FAIL");
	return ok == rounds ? 0 : 1;
}
