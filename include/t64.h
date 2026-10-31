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

#ifndef T64_H
#define T64_H

#include <cstdint>
#include <cstddef>
#include "ff.h"

#pragma pack(push)
#pragma pack(1)
typedef struct
{
    char description[32];
    uint16_t version;
    uint16_t dir_entries;
    uint16_t used_entries;
    uint16_t reserved;
    char user_description[24];
} T64_HEADER;

typedef struct
{
    uint8_t type;            // T64_ENTRY_TYPE
    uint8_t file_type;       // 1541 file type
    uint16_t start_address;
    uint16_t end_address;
    uint16_t reserved;
    uint32_t file_offset;
    uint32_t reserved2;
    char filename[16];
} T64_ENTRY;
#pragma pack(pop)

typedef enum
{
    T64_FREE_ENTRY          = 0x00,
    T64_NORMAL_TAPE_FILE,
    T64_TAPE_WITH_HEADER,
    T64_MEMORY_SNAPSHOT,
    T64_TAPE_BLOCK,
    T64_DIGITIZED_STREAM
} T64_ENTRY_TYPE;

typedef struct
{
    FIL file;
    T64_HEADER header;
    T64_ENTRY entry;
    uint16_t next_entry;
} T64_IMAGE;

#define T64_VERSION_1_0 0x100
#define T64_VERSION_2_0 0x200

bool t64_open(T64_IMAGE *image, const char *filename);
bool t64_close(T64_IMAGE *image);
void t64_rewind_dir(T64_IMAGE *image);
bool t64_read_dir(T64_IMAGE *image);
size_t t64_read_prg(T64_IMAGE *image, uint8_t *buf, size_t buf_size);

#endif
