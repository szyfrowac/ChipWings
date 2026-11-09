/*
 * examples/linux/mavlink_link.c
 *
 * mavlink_link [seconds] [takeoff]
 *
 * Talks MAVLink to PX4 on the flight-control hart over /dev/ttyRPMSG0 (Linux
 * rpmsg_tty <-> the raw "rpmsg-tty" endpoint in the PX4 board, rpmsg_tty.c).
 * Acts like a companion computer:
 *
 *   - sends a HEARTBEAT every second. This is also what lets PX4 send anything
 *     at all: OpenAMP only learns Linux's endpoint address from the first
 *     message Linux sends;
 *   - listens for HEARTBEAT, ATTITUDE, LOCAL_POSITION_NED and COMMAND_ACK;
 *   - counts CRC errors and gaps in the MAVLink sequence number (every
 *     message from PX4 on the link increments it, so a gap means a lost message);
 *   - with "takeoff": sends arm and takeoff commands once PX4 is heard, then
 *     reports the altitude it reaches.
 *
 * Prints "mavlink_link: PASS" or "mavlink_link: FAIL" as its last line.
 * Hand-written MAVLink 2 framing (no library needed), CRC_EXTRA values from
 * the common dialect.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "mavlink_crcs.h"

#define DEV "/dev/ttyRPMSG0"

#define MSG_HEARTBEAT           0
#define MSG_ATTITUDE            30
#define MSG_LOCAL_POSITION_NED  32
#define MSG_COMMAND_LONG        76
#define MSG_COMMAND_ACK         77

#define CMD_NAV_TAKEOFF         22
#define CMD_COMPONENT_ARM_DISARM 400

#define MY_SYSID  255
#define MY_COMPID 190
#define PX4_SYSID 1

/* CRC_EXTRA of a message id, or -1 if the id is not in the common dialect */
static int crc_extra(uint32_t id)
{
	size_t lo = 0, hi = sizeof(mavlink_crcs) / sizeof(mavlink_crcs[0]);

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;

		if (mavlink_crcs[mid].id == id)
			return mavlink_crcs[mid].extra;
		if (mavlink_crcs[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
}

static uint16_t crc_accumulate(uint8_t b, uint16_t crc)
{
	uint8_t tmp = b ^ (uint8_t)(crc & 0xff);

	tmp ^= (tmp << 4);
	return (crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4);
}

static uint16_t crc_calc(const uint8_t *p, size_t n, uint8_t extra)
{
	uint16_t crc = 0xffff;

	while (n--)
		crc = crc_accumulate(*p++, crc);
	return crc_accumulate(extra, crc);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- sending ---- */

static uint8_t tx_seq;

static int send_msg(int fd, uint32_t id, const uint8_t *payload, uint8_t len)
{
	uint8_t f[10 + 255 + 2];
	uint16_t crc;

	/* MAVLink 2 drops trailing zero bytes of the payload (keeping at least one) */
	while (len > 1 && payload[len - 1] == 0)
		len--;

	f[0] = 0xfd;
	f[1] = len;
	f[2] = 0;
	f[3] = 0;
	f[4] = tx_seq++;
	f[5] = MY_SYSID;
	f[6] = MY_COMPID;
	f[7] = id & 0xff;
	f[8] = (id >> 8) & 0xff;
	f[9] = (id >> 16) & 0xff;
	memcpy(f + 10, payload, len);
	crc = crc_calc(f + 1, 9 + len, crc_extra(id));
	f[10 + len] = crc & 0xff;
	f[11 + len] = crc >> 8;
	return write(fd, f, 12 + len) == 12 + len ? 0 : -1;
}

static void send_heartbeat(int fd)
{
	uint8_t p[9] = { 0 };

	p[4] = 6;  /* MAV_TYPE_GCS */
	p[5] = 8;  /* MAV_AUTOPILOT_INVALID */
	p[8] = 3;  /* MAVLink version */
	send_msg(fd, MSG_HEARTBEAT, p, sizeof(p));
}

static void send_command(int fd, uint16_t cmd, float p1, float p7)
{
	uint8_t p[33] = { 0 };
	float params[7] = { p1, 0, 0, NAN, 0, 0, p7 };

	memcpy(p, params, sizeof(params));
	memcpy(p + 28, &cmd, 2);
	p[30] = PX4_SYSID;
	p[31] = 1;       /* MAV_COMP_ID_AUTOPILOT1 */
	send_msg(fd, MSG_COMMAND_LONG, p, sizeof(p));
}

/* ---- receiving ---- */

struct stats {
	unsigned heartbeat, attitude, position, ack, crc_err, seq_gaps, lost, frames;
	int have_seq;
	uint8_t last_seq;
	float last_z, last_roll, last_pitch;
	int ack_result[2];     /* result of the arm and takeoff commands, -1 if none */
};

static void handle(struct stats *s, uint32_t id, const uint8_t *pl, uint8_t len)
{
	uint8_t buf[64] = { 0 };   /* zero-extend: MAVLink 2 truncates trailing zeros */

	memcpy(buf, pl, len < sizeof(buf) ? len : sizeof(buf));

	switch (id) {
	case MSG_HEARTBEAT:
		s->heartbeat++;
		break;
	case MSG_ATTITUDE:
		s->attitude++;
		memcpy(&s->last_roll, buf + 4, 4);
		memcpy(&s->last_pitch, buf + 8, 4);
		break;
	case MSG_LOCAL_POSITION_NED:
		s->position++;
		memcpy(&s->last_z, buf + 12, 4);
		break;
	case MSG_COMMAND_ACK: {
		uint16_t cmd;

		s->ack++;
		memcpy(&cmd, buf, 2);
		if (cmd == CMD_COMPONENT_ARM_DISARM)
			s->ack_result[0] = buf[2];
		else if (cmd == CMD_NAV_TAKEOFF)
			s->ack_result[1] = buf[2];
		break;
	}
	}
}

/* Byte-wise MAVLink 2 frame parser */
static void parse(struct stats *s, const uint8_t *data, size_t n)
{
	static uint8_t f[10 + 255 + 2];
	static size_t have, want;

	while (n--) {
		uint8_t c = *data++;

		if (have == 0) {
			if (c != 0xfd)
				continue;   /* skip to the next start byte */
			want = 0;
		}
		f[have++] = c;
		if (have == 2)
			want = 12 + f[1] + ((f[2] & 1) ? 13 : 0);   /* signed frames carry 13 more bytes */
		if (want && have == want) {
			uint32_t id = f[7] | (f[8] << 8) | (f[9] << 16);
			uint8_t len = f[1];
			uint16_t crc = f[10 + len] | (f[11 + len] << 8);

			have = 0;
			s->frames++;
			if (crc_extra(id) < 0) {
				s->crc_err++;   /* not a common-dialect id: a corrupted frame */
				continue;
			}
			if (crc_calc(f + 1, 9 + len, crc_extra(id)) != crc) {
				s->crc_err++;
				continue;
			}
			if (f[5] == PX4_SYSID) {
				if (s->have_seq && (uint8_t)(f[4] - s->last_seq) != 1) {
					s->seq_gaps++;
					s->lost += (uint8_t)(f[4] - s->last_seq - 1);
				}
				s->have_seq = 1;
				s->last_seq = f[4];
			}
			handle(s, id, f + 10, len);
		}
		if (have > sizeof(f))
			have = 0;
	}
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 60;
	int takeoff = argc > 2 && !strcmp(argv[2], "takeoff");
	struct stats s = { .ack_result = { -1, -1 } };
	struct termios t;
	uint8_t buf[512];
	double start, next_hb, t_arm = 0, t_takeoff = 0;
	int fd, ok = 1, armed_sent = 0, takeoff_sent = 0, ack_reported = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);   /* progress lines reach a log file while the program runs */

	fd = open(DEV, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "mavlink_link: cannot open %s: %s\n", DEV, strerror(errno));
		printf("mavlink_link: FAIL\n");
		return 1;
	}
	tcgetattr(fd, &t);
	cfmakeraw(&t);
	tcsetattr(fd, TCSANOW, &t);

	start = now();
	next_hb = start;
	printf("mavlink_link: %s, %d s%s\n", DEV, secs, takeoff ? ", will arm and take off" : "");

	while (now() - start < secs) {
		fd_set rf;
		struct timeval tv = { 0, 100000 };
		double tnow = now();

		if (tnow >= next_hb) {
			send_heartbeat(fd);
			next_hb += 1.0;
		}

		if (takeoff && s.heartbeat && !armed_sent && tnow - start > 5) {
			printf("mavlink_link: PX4 heard, arming\n");
			send_command(fd, CMD_COMPONENT_ARM_DISARM, 1, 0);
			armed_sent = 1;
			t_arm = tnow;
		}
		if (armed_sent && !takeoff_sent && tnow - t_arm > 3) {
			printf("mavlink_link: arm ack %d, taking off\n", s.ack_result[0]);
			send_command(fd, CMD_NAV_TAKEOFF, 0, NAN);
			takeoff_sent = 1;
			t_takeoff = tnow;
		}

		if (takeoff_sent && !ack_reported && s.ack_result[1] != -1) {
			printf("mavlink_link: takeoff ack %d\n", s.ack_result[1]);
			ack_reported = 1;
		}

		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		if (select(fd + 1, &rf, NULL, NULL, &tv) > 0) {
			ssize_t n = read(fd, buf, sizeof(buf));

			if (n > 0)
				parse(&s, buf, n);
		}
	}

	printf("mavlink_link: %u heartbeats, %u attitude, %u position, %u acks, %u frames\n",
	       s.heartbeat, s.attitude, s.position, s.ack, s.frames);
	printf("mavlink_link: crc errors %u, sequence gaps %u (%u messages lost)\n", s.crc_err, s.seq_gaps, s.lost);

	if (s.heartbeat + 3 < (unsigned)secs) {
		printf("mavlink_link: expected about %d heartbeats\n", secs);
		ok = 0;
	}
	if (s.attitude == 0) {
		printf("mavlink_link: no ATTITUDE received\n");
		ok = 0;
	}
	if (s.seq_gaps || s.crc_err) {
		ok = 0;
	}
	if (takeoff) {
		printf("mavlink_link: acks: arm %d takeoff %d, z = %.2f m after %.0f s\n",
		       s.ack_result[0], s.ack_result[1], s.last_z, takeoff_sent ? now() - t_takeoff : 0.0);
		if (s.ack_result[0] != 0 || s.ack_result[1] != 0 || s.last_z > -2.0)
			ok = 0;
	}
	printf("mavlink_link: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
