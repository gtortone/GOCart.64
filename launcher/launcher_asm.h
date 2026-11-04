/*
 * Copyright (c) 2019-2024 Kim Jørgensen
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <stdint.h>
#include <stdbool.h>

#ifndef _LAUNCHER_ASM_H_
#define _LAUNCHER_ASM_H_

void __fastcall__ kff_send_size_data(void *data, uint8_t size);
void __fastcall__ kff_receive_data(void *data, uint16_t size);
uint8_t __fastcall__ kff_send_reply(uint8_t reply);

void kff_wait_for_sync(void);

void wait_for_reset(void);
uint8_t is_c128(void);

// joysticks in port 1 and 2 (JOY_*_MASK bits); port 1 bits also ORed
// into joy1_bits
uint8_t read_joys(void);
extern uint8_t joy1_bits;

// selection bar (raster interrupt): background color of screen row
// selbar_row (SELBAR_OFF: none) set to selbar_color
#define SELBAR_OFF 0xff
void selbar_init(void);
extern uint8_t selbar_row;
extern uint8_t selbar_color;
extern uint8_t selbar_bg;

#endif /* _LAUNCHER_ASM_H_ */
