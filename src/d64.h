/*
 * D64/D71/D81 disk image reader (directory listing and PRG extraction)
 *
 * Read only, no drive emulation. Based on the disk layout used by
 * Kung Fu Flash (d64.c, derived from d642prg V2.09 by Covert Bitops and
 * iAN CooG/HokutoForce^TWT^HVSC).
 */

#ifndef D64_H
#define D64_H

#include <cstdint>
#include <cstddef>
#include "ff.h"

typedef enum
{
    D64_TYPE_UNKNOWN = 0,
    D64_TYPE_D64,
    D64_TYPE_D71,
    D64_TYPE_D81
} D64_TYPE;

#define D64_SECTOR_LEN  256

typedef struct
{
    uint8_t type;            // file type (CBM_FILE_*)
    uint8_t track;           // first data block
    uint8_t sector;
    char filename[16];       // padded with $A0
    uint16_t blocks;
} D64_ENTRY;

typedef struct
{
    FIL file;
    uint8_t type;            // D64_TYPE
    uint8_t tracks;

    char diskname[24];       // name, id and DOS type for the "*" line
    uint8_t diskname_len;

    // directory reader
    uint8_t first_dir_track;
    uint8_t first_dir_sector;
    uint8_t next_track;
    uint8_t next_sector;
    uint8_t entry_no;        // next entry in sector_buf (0-8)
    uint8_t dir_sectors;     // sectors read in this pass (loop guard)
    uint8_t sector_buf[D64_SECTOR_LEN];

    D64_ENTRY entry;         // last entry read
} D64_IMAGE;

uint8_t d64_get_type(FSIZE_t size);
bool d64_open(D64_IMAGE *image, const char *filename);
bool d64_close(D64_IMAGE *image);
void d64_rewind_dir(D64_IMAGE *image);
bool d64_read_dir(D64_IMAGE *image);
size_t d64_read_prg(D64_IMAGE *image, uint8_t *buf, size_t buf_size);

#endif
