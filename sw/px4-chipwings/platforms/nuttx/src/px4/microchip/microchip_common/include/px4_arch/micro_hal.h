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
#pragma once

#include <px4_platform/micro_hal.h>

__BEGIN_DECLS

#include <mpfs_gpio.h>
#include <mpfs_spi.h>
#include <mpfs_i2c.h>

/* MPFS has a 128-bit device serial number (DSN), read from the system
 * controller with mpfs_read_dsn(). STM32 has a 96-bit UID at a fixed address.
 * board_get_uuid32() returns the DSN as four words in STM32's order: word 0
 * holds the least significant 32 bits. There is no legacy display order to
 * keep, so use the correct correlation.
 */
#define PX4_CPU_UUID_BYTE_LENGTH                16
#define PX4_CPU_UUID_WORD32_LENGTH              (PX4_CPU_UUID_BYTE_LENGTH/sizeof(uint32_t))

/* The mfguid will be an array of bytes with
 * MSD @ index 0 - LSD @ index PX4_CPU_MFGUID_BYTE_LENGTH-1
 *
 * It will be converted to a string with the MSD on left and LSD on the right most position.
 */
#define PX4_CPU_MFGUID_BYTE_LENGTH              PX4_CPU_UUID_BYTE_LENGTH

#define PX4_CPU_UUID_CORRECT_CORRELATION
#define PX4_CPU_UUID_WORD32_UNIQUE_H            0 /* Least significant digits change the most */
#define PX4_CPU_UUID_WORD32_UNIQUE_M            1 /* Middle significant digits */
#define PX4_CPU_UUID_WORD32_UNIQUE_L            2 /* Most significant digits change the least */

/*                                                  Separator    nnn:nnn:nnnn     2 char per byte           term */
#define PX4_CPU_UUID_WORD32_FORMAT_SIZE         (PX4_CPU_UUID_WORD32_LENGTH-1+(2*PX4_CPU_UUID_BYTE_LENGTH)+1)
#define PX4_CPU_MFGUID_FORMAT_SIZE              ((2*PX4_CPU_MFGUID_BYTE_LENGTH)+1)

/* No battery-backed SRAM on MPFS, so there is nowhere to keep a crash dump
 * across reset and px4_savepanic is not defined (STM32 uses BBSRAM). The
 * logger and board_crashdump check for it.
 */

#define PX4_BUS_OFFSET       1                  /* MPFS buses are 0 based, adjustment needed */
#define px4_spibus_initialize(bus_num_1based)   mpfs_spibus_initialize(PX4_BUS_NUMBER_FROM_PX4(bus_num_1based))

#define px4_i2cbus_initialize(bus_num_1based)   mpfs_i2cbus_initialize(PX4_BUS_NUMBER_FROM_PX4(bus_num_1based))
#define px4_i2cbus_uninitialize(pdev)           mpfs_i2cbus_uninitialize(pdev)

/* mpfs_configgpio has no unconfigure; an input without pulls is the reset state */
#define px4_arch_configgpio(pinset)             mpfs_configgpio(pinset)
#define px4_arch_unconfiggpio(pinset)           mpfs_configgpio(PX4_GPIO_PIN_OFF(pinset))
#define px4_arch_gpioread(pinset)               mpfs_gpioread(pinset)
#define px4_arch_gpiowrite(pinset, value)       mpfs_gpiowrite(pinset, value)
/* mpfs_gpiosetevent also takes level (high/low) triggers; PX4 only uses edges */
#define px4_arch_gpiosetevent(pinset,r,f,e,fp,a)  mpfs_gpiosetevent(pinset,r,f,false,false,e,fp,a)

/* MPFS GPIO has no initial output level in the pinset (STM32's GPIO_OUTPUT_SET
 * and GPIO_OUTPUT_CLEAR), so the _SET and _CLEAR variants are the same; the
 * caller writes the level after configuring the pin.
 */
#define PX4_MAKE_GPIO_INPUT(gpio) (((gpio) & (GPIO_BANK_MASK | GPIO_PIN_MASK)) | (GPIO_INPUT|GPIO_PULLUP))
#define PX4_MAKE_GPIO_EXTI(gpio) (((gpio) & (GPIO_BANK_MASK | GPIO_PIN_MASK)) | (GPIO_INPUT|GPIO_PULLUP|GPIO_IRQ_ENABLE))
#define PX4_MAKE_GPIO_OUTPUT_CLEAR(gpio) (((gpio) & (GPIO_BANK_MASK | GPIO_PIN_MASK)) | (GPIO_OUTPUT|GPIO_BUFFER_ENABLE))
#define PX4_MAKE_GPIO_OUTPUT_SET(gpio) (((gpio) & (GPIO_BANK_MASK | GPIO_PIN_MASK)) | (GPIO_OUTPUT|GPIO_BUFFER_ENABLE))

#define PX4_GPIO_PIN_OFF(def) (((def) & (GPIO_BANK_MASK | GPIO_PIN_MASK)) | (GPIO_INPUT))

/* No CAN bootloader on MPFS, so STM32's CAN filter register locations
 * (crc_HiLOC ... node_id_LOC) are not defined. The HRT counts CLINT mtime.
 */

#define TIMER_HRT_CYCLES_PER_US (MPFS_MSS_RTC_TOGGLE_CLK/1000000)
#define TIMER_HRT_CYCLES_PER_MS (MPFS_MSS_RTC_TOGGLE_CLK/1000)

/* The U54 L1 data cache line is 64 bytes. The caches are coherent between
 * harts, but keep DMA buffers line aligned as STM32 does with its D-cache.
 */
#define PX4_ARCH_DCACHE_ALIGNMENT 64
#define px4_cache_aligned_data() aligned_data(PX4_ARCH_DCACHE_ALIGNMENT)
#define px4_cache_aligned_alloc(s) memalign(PX4_ARCH_DCACHE_ALIGNMENT,(s))


__END_DECLS
