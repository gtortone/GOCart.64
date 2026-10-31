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

#include "t64.h"
#include "filesystem.h"
#include "file_types.h"

bool t64_open(T64_IMAGE *image, const char *filename) {

   if (!file_open(&image->file, filename, FA_READ))
      return false;

   uint32_t len = file_read(&image->file, &image->header, sizeof(T64_HEADER));

   if (len != sizeof(T64_HEADER) || image->header.version < T64_VERSION_1_0 ||
       image->header.version > T64_VERSION_2_0) {
      file_close(&image->file);
      return false;
   }

   image->next_entry = 0;
   return true;
}

bool t64_close(T64_IMAGE *image) {
   return file_close(&image->file);
}

void t64_rewind_dir(T64_IMAGE *image) {
   image->next_entry = 0;
}

static bool t64_read_next(T64_IMAGE *image) {

   if (image->next_entry >= image->header.dir_entries) {
      t64_rewind_dir(image);
      return false;
   }

   FSIZE_t offset = sizeof(T64_HEADER) + image->next_entry * sizeof(T64_ENTRY);

   if (!file_seek(&image->file, offset) ||
       file_read(&image->file, &image->entry, sizeof(T64_ENTRY)) != sizeof(T64_ENTRY))
      return false;

   image->next_entry++;
   return true;
}

// read next normal tape file entry
bool t64_read_dir(T64_IMAGE *image) {

   while (t64_read_next(image)) {
      if (image->entry.type == T64_NORMAL_TAPE_FILE)
         return true;
   }

   return false;
}

// read current entry as PRG (2 byte load address + data), returns PRG size or 0
size_t t64_read_prg(T64_IMAGE *image, uint8_t *buf, size_t buf_size) {

   uint16_t prg_size = image->entry.end_address - image->entry.start_address;

   if (prg_size >= (buf_size - 2) ||
       !file_seek(&image->file, image->entry.file_offset))
      return 0;

   // some T64 files have a wrong end address: read up to the end of file
   uint32_t len = file_read(&image->file, buf + 2, prg_size) + 2;

   // PRG should at least have a 2 byte load address and 1 byte of data
   if (len > 2 && len < 64*1024) {
      buf[0] = image->entry.start_address & 0xFF;
      buf[1] = image->entry.start_address >> 8;
      return len;
   }

   return 0;
}
