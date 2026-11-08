//
// Barebones IR Receiver
//
// Written by Larry Bank (bitbank@pobox.com)
//
// The goal of this code is to use only a $0.25 IR demodulator connected directly to
// an available GPIO pin to decode NEC 32-bit codes from any random remote control.
// The purpose is to allow you to use these ubiquitous controls to make your SBC
// do whatever you want - media control, run programs, act as a keyboard/mouse, etc.

// Unlike the LIRC project, this code doesn't require a kernel driver nor a specialized
// USB IR receiver to function. It can directly decode IR codes in user space by
// just looking at the state of a single GPIO pin. Hopefully this enables new, lower
// cost use cases on more diverse hardware.
//
// This code polls the state of the GPIO pin instead of using interrupts. This is to
// allow the widest possible use on diverse operating systems and hardware. Polling
// could use 100% of one CPU core, but I added a usleep() in the idle loop that waits
// for the first signal edge. This allows it to use less than 2% of 1 CPU core on the
// RPI Zero 2W.
//
// SPDX-FileCopyrightText: 2026 BitBank Software, Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===========================================================================
//
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#ifndef __MEM_ONLY__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#endif // !__MEM_ONLY__
#include <math.h>
#include <time.h>

#ifndef CONSUMER
#define CONSUMER "Consumer"
#endif
static char szChipName[16];
struct gpiod_chip *chip = NULL;
#ifdef GPIOD_API
struct gpiod_line *lines[512];
#else
struct gpiod_line_request *lines[512];
#endif


//
// Define this for your particular hardware. The only requirement is that the GPIO
// pin chosen is not already reserved for another function (e.g. I2C).
// On the RPI Zero 2W (used for testing of v1 of this program), GPIO 17 (Pin 11) is
// a good choice. On the Zero 2W, all GPIO pins are connected to "Chip 0"
//
#define IR_PIN 17
// On the OrangePi Zero 2W, physical pin 11 is GPIO 226
//#define IR_PIN 226
#define CHIP_NUM 0

//
// Returns 0 (low) or 1 (high) for the current pin state
//
int digitalRead(int iPin)
{
   if (iPin == -1 || iPin == 0xff || lines[iPin] == 0) return 0;
#ifdef GPIOD_API // 1.x (old) API
   return gpiod_line_get_value(lines[iPin]);
#else // 2.x (new)
   return gpiod_line_request_get_value(lines[iPin], iPin) == GPIOD_LINE_VALUE_ACTIVE;
#endif
} /* digitalRead() */

void InitPin(int iPin, int iChip)
{
   if (iPin == -1 || iPin == 0xff) return; // invalid pin number
                                           //
   sprintf(szChipName, "gpiochip%d", iChip); // GPIO Chip number

#ifdef GPIOD_API // old 1.6 API
   if (chip == NULL) {
       chip = gpiod_chip_open_by_name(szChipName);
   }
   lines[iPin] = gpiod_chip_get_line(chip, iPin);
   gpiod_line_request_input(lines[iPin], CONSUMER);
   gpiod_line_request_input_flags(lines[iPin], CONSUMER, GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP);

#else // new 2.x API
   struct gpiod_line_settings *settings;
   struct gpiod_line_config *line_cfg;
   struct gpiod_request_config *req_cfg;
   char szChip[32];
   snprintf(szChip, sizeof(szChip)-1, "/dev/%s", szChipName);
   chip = gpiod_chip_open(szChip);
   if (!chip) {
        printf("chip open failed\n");
        return;
   }
   settings = gpiod_line_settings_new();
   if (!settings) {
        printf("line_settings_new failed\n");
        return;
   }
   gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
   gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
   gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

   line_cfg = gpiod_line_config_new();
   if (!line_cfg) return;
   gpiod_line_config_add_line_settings(line_cfg, (const unsigned int *)&iPin, 1, settings);
   req_cfg = gpiod_request_config_new();
   gpiod_request_config_set_consumer(req_cfg, CONSUMER);
   lines[iPin] = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
   gpiod_request_config_free(req_cfg);
   gpiod_line_config_free(line_cfg);
   gpiod_line_settings_free(settings);
   gpiod_chip_close(chip);
#endif
} /* InitPin() */

//
// Return the current elapsed time in microseconds
// Used to time the edge transistions of the IR receiver signal
//
long micros(void)
{
int iTime;
struct timespec res;

    clock_gettime(CLOCK_MONOTONIC, &res);
    iTime = 1000000*res.tv_sec + res.tv_nsec/1000;

    return (long)iTime;
} /* micros() */

//
// NEC "standard" 32-bit decoder
// Call this on the falling edge signal from the IR 38KHz demodulator
//
// It assumes there is a mark+space at the start of each code (5 to 13.5ms total)
// followed by 32 data bits encoded as follows:
// 0 is encoded as 562.5us mark (LOW) followed by 562.5us space (HIGH)
// 1 is encoded as 562.5us mark followed by 1687.5us space
// terminated with a single mark of 562.5us
//
uint32_t getCode(void)
{
  int mark, space, iBit = 0;
  uint32_t u32Bits = 0;
  bool bDone = false, bStarted = false;
  long l, lStartTime = micros();

  while (!bDone && micros() - lStartTime < 68000) { // max time to receive any code is 68ms
     l = micros();
      while (digitalRead(IR_PIN) == 0) { } // mark of a bit
      l = micros() - l; // time of the active pulse (mark) in microseconds
      mark = l / 512; // close estimate of how many 562.5us periods passed
      l = micros();
      while (digitalRead(IR_PIN) == 1 && (micros() - l) < 20000) {} // space of a bit
      l = micros() - l; // time of the inactive period (space) in microseconds
      if (l >= 20000) { // too long of a quiet period == invalid code; exit
        bDone = true;
      } else {
        space = l / 512;
        if (bStarted) {
            u32Bits <<= 1;
            if (space == 3) { // a 1 bit (inactive signal time == 3 x 562.5us)
                u32Bits |= 1;
            }
            iBit++;
            if (iBit == 32) bDone = true;
        } // bStarted
        if (mark >= 3 && space >= 3) bStarted = true; // start of a code is at least 1.5ms + 1.5ms (mark+space)
      }
  } // while collecting bits
  while (digitalRead(IR_PIN) == 0) {
      usleep(250); // wait for the last mark to end
  }
  return u32Bits;
} /* getCode() */

int main(int argc, const char **argv)
{
uint32_t u32Code;

    InitPin(IR_PIN, CHIP_NUM);
    while (1) {
        while (digitalRead(IR_PIN) == 1) { // wait for first falling edge
            usleep(1000);
        }
        u32Code = getCode();
        if (u32Code) {
            printf("Received code: 0x%08x\n", u32Code);
        }
    } // while (1) 
    return 0;
} /* main() */
