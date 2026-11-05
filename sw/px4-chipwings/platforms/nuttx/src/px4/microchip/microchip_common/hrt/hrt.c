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
 * @file hrt.c
 *
 * High-resolution timer callouts and timekeeping.
 *
 * PolarFire SoC version of the STM32 driver (stm32_common/hrt/hrt.c).
 *
 * STM32 uses one timer for both jobs: a free-running counter for the time
 * and a compare channel for the next deadline. MPFS has no MSS timer with a
 * compare channel, so the jobs are split:
 *
 *  - time:      CLINT mtime, 64 bits at 1 MHz, shared by all harts and by
 *               Linux. It never wraps, so there is no base_time tracking.
 *  - deadlines: MSS Timer 1 (selected by HRT_TIMER in board_config.h) in
 *               one-shot mode, loaded with the time left to the deadline.
 *
 * mtimecmp for hart 1 is not used: it drives the NuttX tick, just as SysTick
 * does on STM32. Loading a relative delay adds a little skew between reading
 * the time and starting the timer; the latency histogram below measures it.
 *
 * The callout queue, the hrt_call_* API and the latency code are STM32's,
 * unchanged. PPM input is not supported (no capture channel).
 *
 * We don't use a NuttX driver for the timer; we claim it and drive it
 * directly, as on STM32.
 */

#include <px4_platform_common/px4_config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include <sys/types.h>
#include <stdbool.h>

#include <assert.h>
#include <debug.h>
#include <time.h>
#include <queue.h>
#include <errno.h>
#include <string.h>

#include <board_config.h>
#include <drivers/drv_hrt.h>
#include <px4_platform_common/log.h>


#include "riscv_internal.h"
#include "hardware/mpfs_clint.h"
#include "hardware/mpfs_memorymap.h"
#include "hardware/mpfs_sysreg.h"
#include "hardware/mpfs_timer.h"

#ifdef CONFIG_DEBUG_HRT
#  define hrtinfo _info
#else
#  define hrtinfo(x...)
#endif

#ifdef HRT_TIMER

/* HRT configuration */
#if   HRT_TIMER == 1
# define HRT_TIMER_VALUE	MPFS_MSTIMER_TIM1VALUE_OFFSET
# define HRT_TIMER_LOADVAL	MPFS_MSTIMER_TIM1LOADVAL_OFFSET
# define HRT_TIMER_CONTROL	MPFS_MSTIMER_TIM1CONTROL_OFFSET
# define HRT_TIMER_RIS		MPFS_MSTIMER_TIM1RIS_OFFSET
# define HRT_TIMER_VECTOR	MPFS_IRQ_TIMER1
#elif HRT_TIMER == 2
# define HRT_TIMER_VALUE	MPFS_MSTIMER_TIM2VALUE_OFFSET
# define HRT_TIMER_LOADVAL	MPFS_MSTIMER_TIM2LOADVAL_OFFSET
# define HRT_TIMER_CONTROL	MPFS_MSTIMER_TIM2CONTROL_OFFSET
# define HRT_TIMER_RIS		MPFS_MSTIMER_TIM2RIS_OFFSET
# define HRT_TIMER_VECTOR	MPFS_IRQ_TIMER2
#else
# error HRT_TIMER must be 1 or 2 (MSS Timer 1 or 2)
#endif

# define HRT_TIMER_BASE		MPFS_MSTIMER_LO_BASE
# define HRT_TIMER_POWER_REG	(MPFS_SYSREG_BASE + MPFS_SYSREG_SUBBLK_CLOCK_CR_OFFSET)
# define HRT_TIMER_POWER_BIT	SYSREG_SUBBLK_CLOCK_CR_TIMER
# define HRT_TIMER_RESET_REG	(MPFS_SYSREG_BASE + MPFS_SYSREG_SOFT_RESET_CR_OFFSET)
# define HRT_TIMER_RESET_BIT	SYSREG_SOFT_RESET_CR_TIMER

/* The MSS timers count the APB clock. This is only the expected rate:
 * hrt_tim_init() measures the real one against mtime (Renode runs the timer
 * at 100 MHz, the Icicle Kit at the APB rate HSS sets up). A board can still
 * override the expected value. */
#ifndef HRT_TIMER_CLOCK
# define HRT_TIMER_CLOCK	MPFS_MSS_APB_AHB_CLK
#endif

/*
 * HRT clock must be a multiple of 1MHz greater than 1MHz
 */
#if (HRT_TIMER_CLOCK % 1000000) != 0
# error HRT_TIMER_CLOCK must be a multiple of 1MHz
#endif
#if HRT_TIMER_CLOCK <= 1000000
# error HRT_TIMER_CLOCK must be greater than 1MHz
#endif

/**
 * Minimum/maximum deadlines.
 *
 * These are suitable for use with a 16-bit timer/counter clocked
 * at 1MHz.  The high-resolution timer need only guarantee that it
 * not wrap more than once in the 50ms period for absolute time to
 * be consistently maintained.
 *
 * The minimum deadline must be such that the time taken between
 * reading a time and writing a deadline to the timer cannot
 * result in missing the deadline.
 */
#define HRT_INTERVAL_MIN	50

/* How long hrt_tim_init() counts the MSS timer against mtime */
#define HRT_CALIBRATION_US	1000
#define HRT_INTERVAL_MAX	50000

/*
 * Scaling factor(s) for the free-running counter; convert an input
 * in counts to a time in microseconds.
 */
#define HRT_COUNTER_SCALE(_c)	(_c)

/*
 * Timer register accessors
 */
#define REG(_reg)	(*(volatile uint32_t *)(HRT_TIMER_BASE + _reg))

#define rVALUE		REG(HRT_TIMER_VALUE)
#define rLOADVAL	REG(HRT_TIMER_LOADVAL)
#define rCONTROL	REG(HRT_TIMER_CONTROL)
#define rRIS		REG(HRT_TIMER_RIS)

/* one-shot: count down from LOADVAL once, interrupt at zero, then stop */
#define CONTROL_HRT	(MPFS_MSTIMER_ENABLE_MASK | MPFS_MSTIMER_MODE_MASK | MPFS_MSTIMER_INTEN_MASK)

/* The free-running counter: CLINT mtime at 1 MHz */
#define rMTIME		(*(volatile uint64_t *)MPFS_CLINT_MTIME)

/*
 * Queue of callout entries.
 */
static struct sq_queue_s	callout_queue;

/* latency baseline (last compare value applied) */
static uint16_t			latency_baseline;

/* timer count at interrupt (for latency purposes) */
static uint16_t			latency_actual;

/* MSS timer counts per microsecond, measured at init (STM32 needs no such
 * thing: its counter and compare share one clock) */
static uint32_t			timer_counts_per_us = HRT_TIMER_CLOCK / 1000000;

/* latency histogram */
const uint16_t latency_bucket_count = LATENCY_BUCKET_COUNT;
const uint16_t latency_buckets[LATENCY_BUCKET_COUNT] = { 1, 2, 5, 10, 20, 50, 100, 1000 };
__EXPORT uint32_t latency_counters[LATENCY_BUCKET_COUNT + 1];

/* timer-specific functions */
static void		hrt_tim_init(void);
static int		hrt_tim_isr(int irq, void *context, void *arg);
static void		hrt_latency_update(void);
static void		hrt_tim_load(hrt_abstime delay);

/* callout list manipulation */
static void		hrt_call_internal(struct hrt_call *entry,
		hrt_abstime deadline,
		hrt_abstime interval,
		hrt_callout callout,
		void *arg);
static void		hrt_call_enter(struct hrt_call *entry);
static void		hrt_call_reschedule(void);
static void		hrt_call_invoke(void);


int hrt_ioctl(unsigned int cmd, unsigned long arg);
/**
 * Initialise the timer we are going to use.
 *
 * We expect that we'll own one of the two 32-bit MSS timers; the other one
 * stays free. Timer 1 and Timer 2 share one clock and reset bit with the
 * 64-bit mode, which we do not use.
 */
static void
hrt_tim_init(void)
{
	/* claim our interrupt vector */
	irq_attach(HRT_TIMER_VECTOR, hrt_tim_isr, NULL);

	/* clock/power on our timer and take it out of reset. SYSREG is shared
	 * with Linux and the other harts and has no atomic set/clear, so only
	 * write it when the bits are not already right (HSS normally sets them). */
	if ((getreg32(HRT_TIMER_POWER_REG) & HRT_TIMER_POWER_BIT) == 0) {
		modifyreg32(HRT_TIMER_POWER_REG, 0, HRT_TIMER_POWER_BIT);
	}

	if (getreg32(HRT_TIMER_RESET_REG) & HRT_TIMER_RESET_BIT) {
		modifyreg32(HRT_TIMER_RESET_REG, HRT_TIMER_RESET_BIT, 0);
	}

	/* disable the timer, 32-bit mode, clear any stale interrupt */
	rCONTROL = 0;
	putreg32(0, HRT_TIMER_BASE + MPFS_MSTIMER_TIM64MODE_OFFSET);
	rRIS = MPFS_MSTIMER_RIS_MASK;

	/* measure the timer clock: free-run it (periodic, no interrupt) for
	 * HRT_CALIBRATION_US of mtime and count. Rounded to whole MHz. */
	rLOADVAL = UINT32_MAX;
	rCONTROL = MPFS_MSTIMER_ENABLE_MASK;

	hrt_abstime start = rMTIME;
	uint32_t count_start = rVALUE;

	while (rMTIME - start < HRT_CALIBRATION_US) {
	}

	uint32_t counts = count_start - rVALUE;
	hrt_abstime elapsed = rMTIME - start;
	rCONTROL = 0;

	if (counts >= elapsed) {
		timer_counts_per_us = (counts + elapsed / 2) / elapsed;
	}

	PX4_INFO("MSS Timer %d at %u MHz (expected %u)", HRT_TIMER, (unsigned)timer_counts_per_us,
		 (unsigned)(HRT_TIMER_CLOCK / 1000000));

	/* set an initial deadline a little ways off (and record it, so the
	 * first latency sample is measured from it; STM32's baseline 0 matches
	 * its first compare value 1000 only roughly) */
	latency_baseline = (rMTIME + 1000) & 0xffff;
	hrt_tim_load(1000);

	/* enable interrupts */
	up_enable_irq(HRT_TIMER_VECTOR);
}

/**
 * Start the one-shot timer to expire delay microseconds from now.
 *
 * This replaces STM32's write of the absolute deadline to the compare
 * register. Must be called with interrupts disabled.
 */
static void
hrt_tim_load(hrt_abstime delay)
{
	rCONTROL = 0;
	rLOADVAL = (uint32_t)(delay * timer_counts_per_us);
	rCONTROL = CONTROL_HRT;
}

/**
 * Handle the compare interrupt by calling the callout dispatcher
 * and then re-scheduling the next deadline.
 */
static int
hrt_tim_isr(int irq, void *context, void *arg)
{
	uint32_t status;

	/* grab the time for latency tracking purposes */
	latency_actual = rMTIME;

	/* copy interrupt status */
	status = rRIS;

	/* ack the interrupt we just read (write 1 to clear) */
	rRIS = status;

	/* was this a timer tick? */
	if (status & MPFS_MSTIMER_RIS_MASK) {

		/* do latency calculations */
		hrt_latency_update();

		/* run any callouts that have met their deadline */
		hrt_call_invoke();

		/* and schedule the next interrupt */
		hrt_call_reschedule();
	}

	return OK;
}

/**
 * Fetch a never-wrapping absolute time value in microseconds from
 * some arbitrary epoch shortly after system start.
 *
 * mtime is 64 bits wide and read with a single load on RV64, so unlike
 * STM32 there is no wrap to track and no critical section is needed. The
 * epoch is the chip reset, which is also Linux's CLOCK_MONOTONIC source.
 */
hrt_abstime
hrt_absolute_time(void)
{
	return HRT_COUNTER_SCALE(rMTIME);
}

/**
 * Store the absolute time in an interrupt-safe fashion
 */
void
hrt_store_absolute_time(volatile hrt_abstime *t)
{
	irqstate_t flags = px4_enter_critical_section();
	*t = hrt_absolute_time();
	px4_leave_critical_section(flags);
}

/**
 * Initialise the high-resolution timing module.
 */
void
hrt_init(void)
{
	sq_init(&callout_queue);
	hrt_tim_init();
}

/**
 * Call callout(arg) after interval has elapsed.
 */
void
hrt_call_after(struct hrt_call *entry, hrt_abstime delay, hrt_callout callout, void *arg)
{
	hrt_call_internal(entry,
			  hrt_absolute_time() + delay,
			  0,
			  callout,
			  arg);
}

/**
 * Call callout(arg) at calltime.
 */
void
hrt_call_at(struct hrt_call *entry, hrt_abstime calltime, hrt_callout callout, void *arg)
{
	hrt_call_internal(entry, calltime, 0, callout, arg);
}

/**
 * Call callout(arg) every period.
 */
void
hrt_call_every(struct hrt_call *entry, hrt_abstime delay, hrt_abstime interval, hrt_callout callout, void *arg)
{
	hrt_call_internal(entry,
			  hrt_absolute_time() + delay,
			  interval,
			  callout,
			  arg);
}

static void
hrt_call_internal(struct hrt_call *entry, hrt_abstime deadline, hrt_abstime interval, hrt_callout callout, void *arg)
{
	irqstate_t flags = px4_enter_critical_section();

	/* if the entry is currently queued, remove it */
	/* note that we are using a potentially uninitialised
	   entry->link here, but it is safe as sq_rem() doesn't
	   dereference the passed node unless it is found in the
	   list. So we potentially waste a bit of time searching the
	   queue for the uninitialised entry->link but we don't do
	   anything actually unsafe.
	*/
	if (entry->deadline != 0) {
		sq_rem(&entry->link, &callout_queue);
	}

	entry->deadline = deadline;
	entry->period = interval;
	entry->callout = callout;
	entry->arg = arg;

	hrt_call_enter(entry);

	px4_leave_critical_section(flags);
}

/**
 * If this returns true, the call has been invoked and removed from the callout list.
 *
 * Always returns false for repeating callouts.
 */
bool
hrt_called(struct hrt_call *entry)
{
	return (entry->deadline == 0);
}

/**
 * Remove the entry from the callout list.
 */
void
hrt_cancel(struct hrt_call *entry)
{
	irqstate_t flags = px4_enter_critical_section();

	sq_rem(&entry->link, &callout_queue);
	entry->deadline = 0;

	/* if this is a periodic call being removed by the callout, prevent it from
	 * being re-entered when the callout returns.
	 */
	entry->period = 0;

	px4_leave_critical_section(flags);
}

static void
hrt_call_enter(struct hrt_call *entry)
{
	struct hrt_call	*call, *next;

	call = (struct hrt_call *)sq_peek(&callout_queue);

	if ((call == NULL) || (entry->deadline < call->deadline)) {
		sq_addfirst(&entry->link, &callout_queue);
		hrtinfo("call enter at head, reschedule\n");
		/* we changed the next deadline, reschedule the timer event */
		hrt_call_reschedule();

	} else {
		do {
			next = (struct hrt_call *)sq_next(&call->link);

			if ((next == NULL) || (entry->deadline < next->deadline)) {
				hrtinfo("call enter after head\n");
				sq_addafter(&call->link, &entry->link, &callout_queue);
				break;
			}
		} while ((call = next) != NULL);
	}

	hrtinfo("scheduled\n");
}

static void
hrt_call_invoke(void)
{
	struct hrt_call	*call;
	hrt_abstime deadline;

	while (true) {
		/* get the current time */
		hrt_abstime now = hrt_absolute_time();

		call = (struct hrt_call *)sq_peek(&callout_queue);

		if (call == NULL) {
			break;
		}

		if (call->deadline > now) {
			break;
		}

		sq_rem(&call->link, &callout_queue);
		hrtinfo("call pop\n");

		/* save the intended deadline for periodic calls */
		deadline = call->deadline;

		/* zero the deadline, as the call has occurred */
		call->deadline = 0;

		/* invoke the callout (if there is one) */
		if (call->callout) {
			hrtinfo("call %p: %p(%p)\n", call, call->callout, call->arg);
			call->callout(call->arg);
		}

		/* if the callout has a non-zero period, it has to be re-entered */
		if (call->period != 0) {
			// re-check call->deadline to allow for
			// callouts to re-schedule themselves
			// using hrt_call_delay()
			if (call->deadline <= now) {
				call->deadline = deadline + call->period;
			}

			hrt_call_enter(call);
		}
	}
}

/**
 * Reschedule the next timer interrupt.
 *
 * This routine must be called with interrupts disabled.
 */
static void
hrt_call_reschedule()
{
	hrt_abstime	now = hrt_absolute_time();
	struct hrt_call	*next = (struct hrt_call *)sq_peek(&callout_queue);
	hrt_abstime	deadline = now + HRT_INTERVAL_MAX;

	/*
	 * Determine what the next deadline will be.
	 *
	 * mtime does not wrap, so HRT_INTERVAL_MAX is only kept to bound the
	 * one-shot load value and to wake up at least every 50 ms, as on STM32.
	 */
	if (next != NULL) {
		hrtinfo("entry in queue\n");

		if (next->deadline <= (now + HRT_INTERVAL_MIN)) {
			hrtinfo("pre-expired\n");
			/* set a minimal deadline so that we call ASAP */
			deadline = now + HRT_INTERVAL_MIN;

		} else if (next->deadline < deadline) {
			hrtinfo("due soon\n");
			deadline = next->deadline;
		}
	}

	hrtinfo("schedule for %u at %u\n", (unsigned)(deadline & 0xffffffff), (unsigned)(now & 0xffffffff));

	/* start the one-shot timer and remember the deadline for latency tracking */
	latency_baseline = deadline & 0xffff;
	hrt_tim_load(deadline - now);
}

static void
hrt_latency_update(void)
{
	uint16_t latency = latency_actual - latency_baseline;
	unsigned	index;

	/* bounded buckets */
	for (index = 0; index < LATENCY_BUCKET_COUNT; index++) {
		if (latency <= latency_buckets[index]) {
			latency_counters[index]++;
			return;
		}
	}

	/* catch-all at the end */
	latency_counters[index]++;
}

void
hrt_call_init(struct hrt_call *entry)
{
	memset(entry, 0, sizeof(*entry));
}

void
hrt_call_delay(struct hrt_call *entry, hrt_abstime delay)
{
	entry->deadline = hrt_absolute_time() + delay;
}

#endif /* HRT_TIMER */
