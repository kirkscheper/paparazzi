/*
 * Copyright (C) 2014 Freek van Tienen <freek.v.tienen@gmail.com>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifndef CONFIG_BEBOP
#define CONFIG_BEBOP

#define BOARD_BEBOP

#include "std.h"
#include "peripherals/video_device.h"
#include "boards/bebop/mt9f002.h"

/** uart connected to GPS internally */
#define UART1_DEV /dev/ttyPA1
#define GPS_UBX_ENABLE_NMEA_DATA_MASK 0xff
/** FTDI cable for stereoboard or external GPS */
#define UART2_DEV /dev/ttyUSB0

/* Default actuators driver */
#define DEFAULT_ACTUATORS "boards/bebop/actuators.h"
#define ActuatorDefaultSet(_x,_y) ActuatorsBebopSet(_x,_y)
#define ActuatorsDefaultInit() ActuatorsBebopInit()
#define ActuatorsDefaultCommit() ActuatorsBebopCommit()

/* Cameras */
extern struct video_config_t bottom_camera;
extern struct video_config_t front_camera;

/* ISP */
struct mt9f002_t mt9f002;

/* by default activate onboard baro */
#ifndef USE_BARO_BOARD
#define USE_BARO_BOARD 1
#endif

/** Time constant for baro low pass filter
 * Default should provide cut-off freq of 1/(2*pi*tau) ~= 4Hz
 */
#ifndef BB_MS5611_FILTER_CONST
#define BB_MS5611_FILTER_CONST 0.04
#endif

/* The ADC from the sonar */
#if USE_ADC0
#define ADC0_ID             0
#define ADC0_CHANNELS       2
#define ADC0_CHANNELS_CNT   1
#define ADC0_BUF_LENGTH     8192
#endif

/* The SPI from the sonar */
#if USE_SPI0
#define SPI0_MODE           0
#define SPI0_BITS_PER_WORD  8
#define SPI0_MAX_SPEED_HZ   320000
#endif

#endif /* CONFIG_BEBOP */
