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
 * @file board_config.h
 *
 * Chipwings flight-control core: PX4 on PolarFire SoC U54 hart 1.
 * Same kinds of defines as boards/px4/fmu-v6x; most are absent because hart 1
 * owns no pins yet (SIH in Phase 5, fabric PWM in Phase 6, sensors in Phase 7).
 */

#pragma once

/****************************************************************************************************
 * Included Files
 ****************************************************************************************************/

#include <px4_platform_common/px4_config.h>
#include <nuttx/compiler.h>
#include <stdint.h>


#include <mpfs_gpio.h>

/****************************************************************************************************
 * Definitions
 ****************************************************************************************************/

/* No LEDs, PWM outputs, ADC or hardware versioning on hart 1. No I2C or SPI
 * either, so there is no i2c.cpp or spi.cpp until the sensors arrive. */

/* High-resolution timer: counter is CLINT mtime, compare is MSS Timer 1 (PLIC 82) */
#define HRT_TIMER               1  /* use MSS Timer 1 for the HRT deadlines */

#define BOARD_ENABLE_CONSOLE_BUFFER
/* dmesg keeps the whole boot (the 4 KiB default loses the start of it); RAM is plentiful */
#define BOARD_CONSOLE_BUFFER_SIZE (1024*32)

#define PX4_GPIO_INIT_LIST { }

__BEGIN_DECLS

/****************************************************************************************************
 * Public Types
 ****************************************************************************************************/

/****************************************************************************************************
 * Public data
 ****************************************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************************************
 * Public Functions
 ****************************************************************************************************/

/****************************************************************************
 * Name: board_rpmsg_initialize
 *
 * Description:
 *   Start the IHC and RPMsg (rptun) link to Linux on harts 2-4 (rpmsg.c).
 *
 ****************************************************************************/

int board_rpmsg_initialize(void);

#include <px4_platform_common/board_common.h>

#endif /* __ASSEMBLY__ */

__END_DECLS
