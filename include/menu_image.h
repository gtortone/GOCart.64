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
 *
 * Ported to GoCart.64
 */

#ifndef MENU_IMAGE_H
#define MENU_IMAGE_H

#include <cstdint>

// 1541 file types (directory entry type byte)
#define CBM_FILE_DEL        0x00
#define CBM_FILE_PRG        0x02
#define CBM_FILE_LOCKED     0x40
#define CBM_FILE_NO_SPLAT   0x80

// helpers to format T64/D64 directory lines for the launcher (ELEMENT_LENGTH)
void image_sanitize_name_pad(char *dest, const char *src, uint8_t size);
void image_format_diskname(char *buffer, const char *name, uint8_t name_len);
void image_format_entry(char *buffer, uint16_t blocks, const char *filename, uint8_t type);

// copy a 16 char CBM file name, stopping at the $A0 padding
void image_copy_name(char *dest, const char *src);

// T64 and D64 menus are never open at the same time: they share their state
#include "menu_t64.h"
#include "menu_d64.h"

typedef union
{
    T64_STATE t64;
    D64_STATE d64;
} IMAGE_STATE;

extern IMAGE_STATE image_state;

#endif
