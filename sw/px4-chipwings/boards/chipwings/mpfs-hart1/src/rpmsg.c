/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @file rpmsg.c
 *
 * RPMsg to Linux on U54 harts 2-4 over the Mi-V IHC. In C because the
 * OpenAMP/libmetal headers need <atomic> from C++, which PX4's NuttX C++
 * build (-nostdinc++) does not have.
 *
 * The icicle board did this in mpfs_bringup() (mpfs_ihc_init) and the Phase 4
 * cwinit app started rptun. The vrings and the resource table are at fixed
 * addresses (script.ld, CONFIG_MPFS_CH1_VRING*_ADDR, the Linux device tree).
 *
 * PX4 must not depend on Linux: as the virtio device, OpenAMP's
 * rpmsg_init_vdev() waits until Linux (the driver) sets DRIVER_OK, which never
 * happens if Linux does not boot. So rptun is started from its own low
 * priority thread, not from board init; PX4 boots either way and the link
 * comes up whenever Linux attaches.
 */

#include "board_config.h"

#include <syslog.h>

#include <nuttx/kthread.h>

#ifdef CONFIG_MPFS_IHC_CLIENT
#  include "mpfs_ihc.h"
#endif

#ifdef CONFIG_RPTUN
#  include <nuttx/rptun/rptun.h>
#endif

/* rptun device registered by mpfs_ihc.c (MPFS_RPTUN_CPU_NAME) */

#define RPTUN_CPU_NAME "mpfs-ihc"

/* Waiting for Linux is a sleep loop; keep it below every PX4 task */

#define RPMSG_START_PRIORITY  10
#define RPMSG_START_STACKSIZE 2048

#if defined(CONFIG_MPFS_IHC_CLIENT) && defined(CONFIG_RPTUN)
static int rpmsg_start_thread(int argc, char *argv[])
{
	int ret = rptun_boot(RPTUN_CPU_NAME);

	if (ret < 0) {
		syslog(LOG_ERR, "[rpmsg] rptun %s start failed: %d\n", RPTUN_CPU_NAME, ret);

	} else {
		syslog(LOG_INFO, "[rpmsg] Linux attached (%s)\n", RPTUN_CPU_NAME);
	}

	return ret;
}
#endif

int board_rpmsg_initialize(void)
{
	int ret = 0;

#ifdef CONFIG_MPFS_IHC_CLIENT
	ret = mpfs_ihc_init();

	if (ret < 0) {
		syslog(LOG_ERR, "[boot] IHC init failed: %d\n", ret);
		return ret;
	}

#  ifdef CONFIG_RPTUN
	ret = kthread_create("rpmsg_start", RPMSG_START_PRIORITY, RPMSG_START_STACKSIZE, rpmsg_start_thread, NULL);

	if (ret < 0) {
		syslog(LOG_ERR, "[boot] rpmsg_start thread failed: %d\n", ret);
	}

#  endif
#endif

	return ret < 0 ? ret : 0;
}
