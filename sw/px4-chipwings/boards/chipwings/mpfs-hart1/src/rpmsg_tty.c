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
 * @file rpmsg_tty.c
 *
 * /dev/ttyRPMSG0 for MAVLink to Linux: a NuttX tty on an RPMsg endpoint that
 * carries raw bytes, which is what Linux's rpmsg_tty driver speaks.
 *
 * NuttX's own uart_rpmsg.c is modelled the same way (uart_dev_t with the DMA
 * hooks) but wraps every message in a header (command, result, cookie, count)
 * that only another NuttX understands; Linux's rpmsg_tty would hand that
 * header to the application as data. So this is uart_rpmsg.c without the
 * header and without the write-response / wakeup handshake.
 *
 * The endpoint is named "rpmsg-tty": Linux binds rpmsg_tty by that exact
 * name and creates /dev/ttyRPMSGn when the name service announcement arrives.
 *
 * OpenAMP learns Linux's endpoint address from the first message Linux sends
 * (the announcement carries only ours), so nothing can be sent to Linux until
 * it has written to its tty once. Until then, and whenever Linux has been silent
 * for RPMSG_TTY_LINK_TIMEOUT_S, outgoing bytes are discarded (stale telemetry is
 * of no use, and a dead peer must not cost PX4 any time). Linux-side tools must
 * therefore speak first and keep talking (mavlink_link sends a HEARTBEAT every second).
 *
 * Bytes from Linux arrive in the endpoint callback and go straight into the
 * tty's RX buffer; when the buffer is full the excess is dropped, MAVLink
 * resynchronises on its own.
 */

#include "board_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/rpmsg/rpmsg.h>
#include <nuttx/serial/serial.h>

#define RPMSG_TTY_CPU_NAME   "mpfs-ihc"   /* rptun device, see rpmsg.c */
#define RPMSG_TTY_EPT_NAME   "rpmsg-tty"  /* what Linux's rpmsg_tty matches */
#define RPMSG_TTY_DEV_PATH   "/dev/ttyRPMSG0"
#define RPMSG_TTY_BUF_SIZE   2048

/* Linux counts as gone when nothing has arrived from it for this long. A MAVLink peer sends a
 * heartbeat every second. Sending to a dead peer is not free: the IHC never acknowledges, and
 * every rptun notification then busy-waits through its whole retry budget (about a second on the
 * board), which would stall the MAVLink thread on every message.
 */
#define RPMSG_TTY_LINK_TIMEOUT_S 3

struct rpmsg_tty_priv_s {
	struct rpmsg_endpoint ept;
	mutex_t               lock;
	volatile clock_t      last_rx;    /* tick of the last message from Linux, 0 = none yet */
	const void           *recv_data;  /* valid only inside the endpoint callback */
	size_t                recv_len;
};

/* uart_ops_s hooks that need no work */

static int  rpmsg_tty_setup(struct uart_dev_s *dev)    { return OK; }
static void rpmsg_tty_shutdown(struct uart_dev_s *dev) { }
static int  rpmsg_tty_attach(struct uart_dev_s *dev)   { return OK; }
static void rpmsg_tty_detach(struct uart_dev_s *dev)   { }
static void rpmsg_tty_rxint(struct uart_dev_s *dev, bool enable) { }
static void rpmsg_tty_txint(struct uart_dev_s *dev, bool enable) { }
static void rpmsg_tty_dmarxfree(struct uart_dev_s *dev) { }
static bool rpmsg_tty_txempty(struct uart_dev_s *dev)  { return true; }

static bool rpmsg_tty_rxflowcontrol(struct uart_dev_s *dev, unsigned int nbuffered, bool upper)
{
	return false;
}

static bool rpmsg_tty_link_up(struct rpmsg_tty_priv_s *priv)
{
	clock_t last = priv->last_rx;

	return is_rpmsg_ept_ready(&priv->ept) && last != 0 &&
	       (clock_systime_ticks() - last) < SEC2TICK(RPMSG_TTY_LINK_TIMEOUT_S);
}

/* Move everything the tty has queued into RPMsg buffers. Called with priv->lock held. */
static void rpmsg_tty_flush_locked(struct uart_dev_s *dev)
{
	struct rpmsg_tty_priv_s *priv = dev->priv;

	if (!rpmsg_tty_link_up(priv)) {
		/* Linux has not spoken yet, or has gone quiet: nobody to send to. Telemetry is worthless
		 * once it is old, and keeping it would hand Linux a burst of stale messages followed by a
		 * gap the moment it attaches, so throw it away. Writers are woken as if it had been sent.
		 */
		if (dev->xmit.head != dev->xmit.tail) {
			dev->xmit.tail = dev->xmit.head;
			uart_datasent(dev);
		}

		return;
	}

	while (dev->xmit.head != dev->xmit.tail) {
		int tail = dev->xmit.tail;

		uart_xmitchars_dma(dev);

		if (dev->xmit.tail == tail) {
			break; /* nothing was handed over (no RPMsg buffer free): try again on the next write */
		}
	}
}

static void rpmsg_tty_dmasend(struct uart_dev_s *dev)
{
	struct rpmsg_tty_priv_s *priv = dev->priv;
	struct uart_dmaxfer_s *xfer = &dev->dmatx;
	size_t len = xfer->length + xfer->nlength;
	uint32_t space;
	uint8_t *buf;

	/* Do not block for a free RPMsg buffer: PX4's MAVLink would stall. Dropping is fine. */
	buf = rpmsg_get_tx_payload_buffer(&priv->ept, &space, false);

	if (buf == NULL) {
		xfer->length = xfer->nlength = 0;
		return;
	}

	if (len > space) {
		len = space;
	}

	if (len > xfer->length) {
		memcpy(buf, xfer->buffer, xfer->length);
		memcpy(buf + xfer->length, xfer->nbuffer, len - xfer->length);

	} else {
		memcpy(buf, xfer->buffer, len);
	}

	if (rpmsg_send_nocopy(&priv->ept, buf, len) < 0) {
		rpmsg_release_tx_buffer(&priv->ept, buf);
		xfer->length = xfer->nlength = 0;
		return;
	}

	/* No acknowledgement on the wire, so the bytes are gone as soon as they are queued */
	xfer->nbytes = len;
	uart_xmitchars_done(dev);
}

static void rpmsg_tty_dmareceive(struct uart_dev_s *dev)
{
	struct rpmsg_tty_priv_s *priv = dev->priv;
	struct uart_dmaxfer_s *xfer = &dev->dmarx;
	size_t len = priv->recv_len;
	size_t space = xfer->length + xfer->nlength;

	if (len > space) {
		len = space; /* RX buffer full: drop the rest */
	}

	if (len > xfer->length) {
		memcpy(xfer->buffer, priv->recv_data, xfer->length);
		memcpy(xfer->nbuffer, (const char *)priv->recv_data + xfer->length, len - xfer->length);

	} else {
		memcpy(xfer->buffer, priv->recv_data, len);
	}

	xfer->nbytes = len;
	uart_recvchars_done(dev);
}

static void rpmsg_tty_dmatxavail(struct uart_dev_s *dev)
{
	struct rpmsg_tty_priv_s *priv = dev->priv;

	nxmutex_lock(&priv->lock);
	rpmsg_tty_flush_locked(dev);
	nxmutex_unlock(&priv->lock);
}

static void rpmsg_tty_send(struct uart_dev_s *dev, int ch)
{
	int nexthead = dev->xmit.head + 1;

	if (nexthead >= dev->xmit.size) {
		nexthead = 0;
	}

	if (nexthead != dev->xmit.tail) {
		dev->xmit.buffer[dev->xmit.head] = ch;
		dev->xmit.head = nexthead;
	}

	rpmsg_tty_dmatxavail(dev);
}

static bool rpmsg_tty_txready(struct uart_dev_s *dev)
{
	int nexthead = dev->xmit.head + 1;

	if (nexthead >= dev->xmit.size) {
		nexthead = 0;
	}

	return nexthead != dev->xmit.tail;
}

static int rpmsg_tty_ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv_)
{
	struct uart_dev_s *dev = priv_;
	struct rpmsg_tty_priv_s *priv = dev->priv;

	/* Linux is alive and its address is known now */
	priv->last_rx = clock_systime_ticks();

	nxmutex_lock(&dev->recv.lock);
	priv->recv_data = data;
	priv->recv_len = len;
	uart_recvchars_dma(dev);
	priv->recv_data = NULL;
	nxmutex_unlock(&dev->recv.lock);

	return 0;
}

static void rpmsg_tty_device_created(struct rpmsg_device *rdev, void *priv_)
{
	struct uart_dev_s *dev = priv_;
	struct rpmsg_tty_priv_s *priv = dev->priv;

	if (strcmp(RPMSG_TTY_CPU_NAME, rpmsg_get_cpuname(rdev)) == 0) {
		priv->ept.priv = dev;
		rpmsg_create_ept(&priv->ept, rdev, RPMSG_TTY_EPT_NAME, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
				 rpmsg_tty_ept_cb, NULL);
	}
}

static void rpmsg_tty_device_destroy(struct rpmsg_device *rdev, void *priv_)
{
	struct uart_dev_s *dev = priv_;
	struct rpmsg_tty_priv_s *priv = dev->priv;

	if (priv->ept.priv != NULL && strcmp(RPMSG_TTY_CPU_NAME, rpmsg_get_cpuname(rdev)) == 0) {
		rpmsg_destroy_ept(&priv->ept);
	}

	dev->dmatx.nbytes = dev->dmatx.length + dev->dmatx.nlength;
	uart_xmitchars_done(dev);
}

static const struct uart_ops_s g_rpmsg_tty_ops = {
	.setup         = rpmsg_tty_setup,
	.shutdown      = rpmsg_tty_shutdown,
	.attach        = rpmsg_tty_attach,
	.detach        = rpmsg_tty_detach,
	.rxint         = rpmsg_tty_rxint,
	.rxflowcontrol = rpmsg_tty_rxflowcontrol,
	.dmasend       = rpmsg_tty_dmasend,
	.dmareceive    = rpmsg_tty_dmareceive,
	.dmarxfree     = rpmsg_tty_dmarxfree,
	.dmatxavail    = rpmsg_tty_dmatxavail,
	.send          = rpmsg_tty_send,
	.txint         = rpmsg_tty_txint,
	.txready       = rpmsg_tty_txready,
	.txempty       = rpmsg_tty_txempty,
};

int board_rpmsg_tty_initialize(void)
{
	struct uart_dev_s *dev;
	struct rpmsg_tty_priv_s *priv;
	int ret;

	dev = kmm_zalloc(sizeof(struct uart_dev_s) + sizeof(struct rpmsg_tty_priv_s) + 2 * RPMSG_TTY_BUF_SIZE);

	if (dev == NULL) {
		return -ENOMEM;
	}

	priv = (struct rpmsg_tty_priv_s *)(dev + 1);
	dev->ops         = &g_rpmsg_tty_ops;
	dev->recv.size   = RPMSG_TTY_BUF_SIZE;
	dev->xmit.size   = RPMSG_TTY_BUF_SIZE;
	dev->priv        = priv;
	dev->recv.buffer = (char *)(priv + 1);
	dev->xmit.buffer = dev->recv.buffer + RPMSG_TTY_BUF_SIZE;

	nxmutex_init(&priv->lock);

	/* Before rptun_boot(): the callback must exist when the rpmsg device appears */
	ret = rpmsg_register_callback(dev, rpmsg_tty_device_created, rpmsg_tty_device_destroy, NULL, NULL);

	if (ret < 0) {
		nxmutex_destroy(&priv->lock);
		kmm_free(dev);
		return ret;
	}

	return uart_register(RPMSG_TTY_DEV_PATH, dev);
}

/* drivers_initialize() calls this when CONFIG_RPMSG_UART is set (which we only set to get the serial
 * DMA support). The tty is registered by board_rpmsg_tty_initialize(), once the IHC is up.
 */
void rpmsg_serialinit(void)
{
}
