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

#include <cstring>

#include "menu_image.h"
#include "commands.h"

IMAGE_STATE image_state;

static const char *cbm_types[8] = {"DEL", "SEQ", "PRG", "USR", "REL", "CBM", "DIR", "???"};

// right aligned decimal in 5 chars, digits written left aligned (like KFF)
static void sprint_u16_left(char *buffer, uint16_t val) {

   uint16_t div = 10000;
   bool found = false;

   uint8_t ii = 0;
   for (uint8_t i=0; i<5; i++) {
      uint8_t d = (val / div) % 10;
      if (d || found || i==4) {
         found = true;
         buffer[ii++] = '0' + d;
      } else {
         buffer[4-i] = ' ';
      }
      div /= 10;
   }
}

void image_sanitize_name_pad(char *dest, const char *src, uint8_t size) {

   for (uint8_t i=0; i<size; i++) {
      char c = *src;
      if (c) {
         src++;
         *dest++ = sanitize_char(c);
      } else {
         *dest++ = ' ';
      }
   }
}

void image_format_diskname(char *buffer, const char *name, uint8_t name_len) {

   // Star
   memcpy(buffer, " *     ", 7);
   buffer += 7;

   // Disk name
   image_sanitize_name_pad(buffer, name, name_len);
   buffer += name_len;

   // Entry type
   const uint8_t entry_len = 27 - name_len;
   image_sanitize_name_pad(buffer, "", entry_len);
   buffer += entry_len;
   strcpy(buffer, "--- ");
}

void image_format_entry(char *buffer, uint16_t blocks, const char *filename, uint8_t type) {

   // Blocks
   *buffer++ = ' ';
   sprint_u16_left(buffer, blocks);
   buffer += 5;
   *buffer++ = ' ';

   // File name
   image_sanitize_name_pad(buffer, filename, 16);
   buffer += 16;
   image_sanitize_name_pad(buffer, "", 10);
   buffer += 10;

   // Entry type
   *buffer++ = (type & CBM_FILE_NO_SPLAT) ? ' ' : '*';
   memcpy(buffer, cbm_types[type & 7], 3);
   buffer += 3;
   *buffer++ = (type & CBM_FILE_LOCKED) ? '<' : ' ';
   *buffer = 0;
}

void image_copy_name(char *dest, const char *src) {

   uint8_t i = 0;
   for (; i < 16 && src[i] && (uint8_t)src[i] != 0xA0; i++)
      dest[i] = sanitize_char(src[i]);

   // trailing spaces (T64 padding)
   while (i && dest[i-1] == ' ')
      i--;

   dest[i] = 0;
}
