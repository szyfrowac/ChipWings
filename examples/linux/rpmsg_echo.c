/*
 * examples/linux/rpmsg_echo.c
 *
 * rpmsg_echo [rounds]
 *
 * Sends messages of every size from 1 byte to the largest RPMsg payload to
 * the NuttX endpoint "cw-echo" (NSH: cw_rpmsg_echo) and checks each one comes
 * back byte for byte. Reports latency and throughput.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cw_rpmsg_proto.h"
#include "cw_rpmsg_util.h"

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : CW_RPMSG_MAX_PAYLOAD;
	uint8_t tx[CW_RPMSG_MAX_PAYLOAD], rx[CW_RPMSG_MAX_PAYLOAD];
	int64_t start, t, lat, min = INT64_MAX, max = 0, sum = 0, bytes = 0;
	int i, j, len, n, ok = 0, fd;

	if (rounds <= 0) {
		fprintf(stderr, "usage: rpmsg_echo [rounds]\n");
		return 1;
	}
	fd = cw_rpmsg_open(CW_ECHO_EPT_NAME);
	if (fd < 0) {
		printf("%s: FAIL\n", "rpmsg_echo");
		return 1;
	}

	start = cw_now_ns();
	for (i = 0; i < rounds; i++) {
		len = 1 + i % CW_RPMSG_MAX_PAYLOAD;
		for (j = 0; j < len; j++)
			tx[j] = (uint8_t)(i * 7 + j);

		t = cw_now_ns();
		if (write(fd, tx, (size_t)len) != len) {
			perror("write");
			break;
		}
		n = cw_rpmsg_recv(fd, rx, sizeof(rx), 2000);
		lat = cw_now_ns() - t;
		if (n <= 0) {
			printf("rpmsg_echo: message %d (%d bytes): no echo within 2 s\n", i, len);
			break;
		}
		if (n != len || memcmp(rx, tx, (size_t)len)) {
			printf("rpmsg_echo: message %d: sent %d bytes, got back %d different bytes\n",
			       i, len, n);
			continue;
		}
		ok++;
		bytes += 2 * len;
		sum += lat;
		min = lat < min ? lat : min;
		max = lat > max ? lat : max;
	}
	t = cw_now_ns() - start;
	cw_rpmsg_close(fd);

	if (ok)
		printf("rpmsg_echo: %d/%d messages of 1..%d bytes echoed intact, latency min %lld us, "
		       "avg %lld us, max %lld us, %lld kB/s both ways\n", ok, rounds,
		       rounds < CW_RPMSG_MAX_PAYLOAD ? rounds : CW_RPMSG_MAX_PAYLOAD,
		       (long long)(min / 1000), (long long)(sum / ok / 1000),
		       (long long)(max / 1000), (long long)(bytes * 1000000 / (t > 0 ? t : 1)));
	printf("rpmsg_echo: %s\n", ok == rounds ? "PASS" : "FAIL");
	return ok == rounds ? 0 : 1;
}
