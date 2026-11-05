/*
 * examples/linux/cw_rpmsg_util.c
 *
 * NuttX announces each endpoint it creates; Linux turns the announcement into
 * an rpmsg device /sys/bus/rpmsg/devices/virtio0.<name>.-1.<addr> whose "dst"
 * is the NuttX endpoint address. RPMSG_CREATE_EPT_IOCTL on /dev/rpmsg_ctrl0
 * then creates a local endpoint bound to that address as /dev/rpmsgN.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rpmsg.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cw_rpmsg_util.h"

#define CTRL_DEV     "/dev/rpmsg_ctrl0"
#define RPMSG_BUS    "/sys/bus/rpmsg/devices"
#define RPMSG_CLASS  "/sys/class/rpmsg"

static int read_attr(const char *dir, const char *entry, const char *attr,
		     char *out, size_t len)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/%s", dir, entry, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

/* The NuttX endpoint address of the announced channel called name */
static int find_channel(const char *name, uint32_t *dst)
{
	char value[64];
	struct dirent *d;
	DIR *dir = opendir(RPMSG_BUS);
	int found = -1;

	if (!dir)
		return -1;
	while ((d = readdir(dir)) != NULL) {
		if (d->d_name[0] == '.')
			continue;
		if (read_attr(RPMSG_BUS, d->d_name, "name", value, sizeof(value)) ||
		    strcmp(value, name))
			continue;
		if (read_attr(RPMSG_BUS, d->d_name, "dst", value, sizeof(value)))
			continue;
		*dst = (uint32_t)strtoul(value, NULL, 0);
		found = 0;
		break;
	}
	closedir(dir);
	return found;
}

/* The /dev/rpmsgN rpmsg_char created for our endpoint (name, dst) */
static int find_eptdev(const char *name, uint32_t dst, char *dev, size_t len)
{
	char value[64];
	struct dirent *d;
	DIR *dir = opendir(RPMSG_CLASS);
	int found = -1;

	if (!dir)
		return -1;
	while ((d = readdir(dir)) != NULL) {
		if (strncmp(d->d_name, "rpmsg", 5) || !strncmp(d->d_name, "rpmsg_ctrl", 10))
			continue;
		if (read_attr(RPMSG_CLASS, d->d_name, "name", value, sizeof(value)) ||
		    strcmp(value, name))
			continue;
		if (read_attr(RPMSG_CLASS, d->d_name, "dst", value, sizeof(value)) ||
		    (uint32_t)strtoul(value, NULL, 0) != dst)
			continue;
		snprintf(dev, len, "/dev/%s", d->d_name);
		found = 0;
		break;
	}
	closedir(dir);
	return found;
}

int cw_rpmsg_open(const char *name)
{
	struct rpmsg_endpoint_info info;
	char dev[300];	/* "/dev/" + a directory entry name */
	uint32_t dst;
	int ctrl, fd, tries;

	if (find_channel(name, &dst)) {
		fprintf(stderr, "%s: no RPMsg channel announced by NuttX; is the NuttX "
			"side running?\n", name);
		return -1;
	}

	/* rpmsg_ctrl provides /dev/rpmsg_ctrl0, rpmsg_char the /dev/rpmsgN endpoints */
	if (access(CTRL_DEV, F_OK) && system("modprobe -a rpmsg_ctrl rpmsg_char") != 0) {
		fprintf(stderr, "%s: cannot load rpmsg_ctrl / rpmsg_char\n", name);
		return -1;
	}
	for (tries = 0; tries < 50 && access(CTRL_DEV, F_OK); tries++)
		usleep(20000);
	ctrl = open(CTRL_DEV, O_RDWR);
	if (ctrl < 0) {
		perror(CTRL_DEV);
		return -1;
	}

	memset(&info, 0, sizeof(info));
	strncpy(info.name, name, sizeof(info.name) - 1);
	info.src = 0xffffffff;	/* RPMSG_ADDR_ANY: let Linux pick our address */
	info.dst = dst;
	if (ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, &info) < 0) {
		perror("RPMSG_CREATE_EPT_IOCTL");
		close(ctrl);
		return -1;
	}
	close(ctrl);

	/* udev-less BusyBox: mdev/devtmpfs creates the node shortly after */
	for (tries = 0; tries < 50; tries++) {
		if (!find_eptdev(name, dst, dev, sizeof(dev))) {
			fd = open(dev, O_RDWR);
			if (fd >= 0)
				return fd;
		}
		usleep(20000);
	}
	fprintf(stderr, "%s: endpoint device did not appear\n", name);
	return -1;
}

void cw_rpmsg_close(int fd)
{
	ioctl(fd, RPMSG_DESTROY_EPT_IOCTL);
	close(fd);
}

int cw_rpmsg_recv(int fd, void *buf, int len, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int ret = poll(&p, 1, timeout_ms);

	if (ret <= 0)
		return ret;
	ret = (int)read(fd, buf, (size_t)len);
	return ret < 0 ? -errno : ret;
}
