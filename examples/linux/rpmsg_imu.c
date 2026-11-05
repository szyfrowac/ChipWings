/*
 * examples/linux/rpmsg_imu.c
 *
 * rpmsg_imu [rate_hz] [count]
 *
 * Mission-computer side of a sensor stream: asks the NuttX endpoint "cw-imu"
 * (NSH: cw_rpmsg_imu &) for count samples at rate_hz and receives them. Checks
 * that every sample arrives once and in order, that the NuttX timestamps are
 * spaced at the requested rate, and that the data looks like a level IMU.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cw_rpmsg_proto.h"
#include "cw_rpmsg_util.h"

int main(int argc, char **argv)
{
	uint32_t rate = argc > 1 ? (uint32_t)atoi(argv[1]) : 100;
	uint32_t count = argc > 2 ? (uint32_t)atoi(argv[2]) : 500;
	struct cw_imu_start_s start = { CW_IMU_START, rate, count };
	struct cw_imu_sample_s *s;
	struct cw_imu_done_s *done;
	uint8_t buf[CW_RPMSG_MAX_PAYLOAD];
	uint32_t received = 0, out_of_order = 0, expected_seq = 0;
	uint64_t first_t = 0, last_t = 0;
	double az_sum = 0, period_us, want_us;
	int got_done = 0, n, fd, ok;
	int timeout_ms;

	if (rate == 0 || rate > 1000 || count == 0) {
		fprintf(stderr, "usage: rpmsg_imu [rate_hz 1..1000] [count]\n");
		return 1;
	}
	timeout_ms = (int)(2000 + 2000 / rate);
	fd = cw_rpmsg_open(CW_IMU_EPT_NAME);
	if (fd < 0) {
		printf("%s: FAIL\n", "rpmsg_imu");
		return 1;
	}

	if (write(fd, &start, sizeof(start)) != sizeof(start)) {
		perror("write");
		cw_rpmsg_close(fd);
		return 1;
	}

	while (!got_done) {
		n = cw_rpmsg_recv(fd, buf, sizeof(buf), timeout_ms);
		if (n <= 0) {
			printf("rpmsg_imu: stream stopped after %u samples (no data for %d ms)\n",
			       received, timeout_ms);
			break;
		}
		switch (((struct cw_imu_hdr_s *)buf)->type) {
		case CW_IMU_SAMPLE:
			if (n != sizeof(*s))
				break;
			s = (struct cw_imu_sample_s *)buf;
			if (s->seq != expected_seq)
				out_of_order++;
			expected_seq = s->seq + 1;
			if (received == 0)
				first_t = s->t_ns;
			last_t = s->t_ns;
			az_sum += s->accel[2];
			if (received < 3 || s->seq + 1 == count)
				printf("rpmsg_imu: #%u t=%llu.%06llu s accel %+.3f %+.3f %+.3f m/s^2 "
				       "gyro %+.4f %+.4f %+.4f rad/s\n", s->seq,
				       (unsigned long long)(s->t_ns / 1000000000ULL),
				       (unsigned long long)(s->t_ns / 1000 % 1000000ULL),
				       s->accel[0], s->accel[1], s->accel[2],
				       s->gyro[0], s->gyro[1], s->gyro[2]);
			received++;
			break;
		case CW_IMU_DONE:
			done = (struct cw_imu_done_s *)buf;
			printf("rpmsg_imu: NuttX sent %u samples, %u late wake-ups\n",
			       done->sent, done->late);
			got_done = 1;
			break;
		}
	}
	cw_rpmsg_close(fd);

	want_us = 1e6 / rate;
	period_us = received > 1 ? (double)(last_t - first_t) / 1000.0 / (received - 1) : 0;
	printf("rpmsg_imu: received %u/%u samples, %u out of order, NuttX period %.1f us "
	       "(requested %.1f us), mean accel z %.3f m/s^2\n", received, count, out_of_order,
	       period_us, want_us, received ? az_sum / received : 0.0);

	ok = got_done && received == count && out_of_order == 0 &&
	     (count < 2 || fabs(period_us - want_us) < want_us * 0.05) &&
	     fabs(az_sum / received - 9.81) < 0.1;
	printf("rpmsg_imu: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
