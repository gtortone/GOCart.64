/*
 * D64/D71/D81 disk image reader (directory listing and PRG extraction)
 *
 * Read only, no drive emulation. Based on the disk layout used by
 * Kung Fu Flash (d64.c, derived from d642prg V2.09 by Covert Bitops and
 * iAN CooG/HokutoForce^TWT^HVSC).
 */

#include <cstring>

#include "d64.h"
#include "filesystem.h"
#include "menu_image.h"

#define D64_TRACK_DIR       18      // D64/D71 header 18/0, directory from 18/1
#define D81_TRACK_DIR       40      // D81 header 40/0, directory from 40/3
#define D81_SECTORS         40

#define D64_SECTORS_SIDE    683     // sectors on a 35 track side (D71 2nd side offset)

#define DIR_ENTRIES_SECTOR  8
#define DIR_ENTRY_LEN       32
#define DIR_MAX_SECTORS     40      // loop guard (D81 directory is 37 sectors)

// first sector of each track (1-42) on a 1541 disk side
static const uint16_t d64_track_offset[42] = {
   0x0000, 0x0015, 0x002a, 0x003f, 0x0054, 0x0069, 0x007e, 0x0093,
   0x00a8, 0x00bd, 0x00d2, 0x00e7, 0x00fc, 0x0111, 0x0126, 0x013b,
   0x0150, 0x0165, 0x0178, 0x018b, 0x019e, 0x01b1, 0x01c4, 0x01d7,
   0x01ea, 0x01fc, 0x020e, 0x0220, 0x0232, 0x0244, 0x0256, 0x0267,
   0x0278, 0x0289, 0x029a, 0x02ab, 0x02bc, 0x02cd, 0x02de, 0x02ef,
   0x0300, 0x0311
};

static uint8_t d64_sectors_on_track(uint8_t track) {
   if (track <= 17) return 21;
   if (track <= 24) return 19;
   if (track <= 30) return 18;
   return 17;
}

uint8_t d64_get_type(FSIZE_t size) {

   switch (size) {
      case 174848:   // 35 tracks
      case 175531:   // 35 tracks with error info
      case 196608:   // 40 tracks
      case 197376:   // 40 tracks with error info
      case 205312:   // 42 tracks
      case 206114:   // 42 tracks with error info
         return D64_TYPE_D64;

      case 349696:   // 70 tracks
      case 351062:   // 70 tracks with error info
         return D64_TYPE_D71;

      case 819200:   // 80 tracks
      case 822400:   // 80 tracks with error info
         return D64_TYPE_D81;
   }

   return D64_TYPE_UNKNOWN;
}

static uint8_t d64_get_tracks(FSIZE_t size) {

   switch (size) {
      case 196608:
      case 197376:
         return 40;
      case 205312:
      case 206114:
         return 42;
      case 349696:
      case 351062:
         return 70;
      case 819200:
      case 822400:
         return 80;
   }
   return 35;
}

// byte offset of a sector in the image, -1 if track/sector is not valid
static int32_t d64_sector_offset(D64_IMAGE *image, uint8_t track, uint8_t sector) {

   if (track < 1 || track > image->tracks)
      return -1;

   uint32_t index;

   if (image->type == D64_TYPE_D81) {
      if (sector >= D81_SECTORS)
         return -1;
      index = (uint32_t)(track - 1) * D81_SECTORS + sector;

   } else if (image->type == D64_TYPE_D71 && track > 35) {
      uint8_t t = track - 35;
      if (sector >= d64_sectors_on_track(t))
         return -1;
      index = D64_SECTORS_SIDE + d64_track_offset[t - 1] + sector;

   } else {
      if (sector >= d64_sectors_on_track(track))
         return -1;
      index = d64_track_offset[track - 1] + sector;
   }

   return (int32_t)(index * D64_SECTOR_LEN);
}

static bool d64_read_sector(D64_IMAGE *image, uint8_t track, uint8_t sector, uint8_t *buf) {

   int32_t offset = d64_sector_offset(image, track, sector);

   return offset >= 0 &&
          file_seek(&image->file, offset) &&
          file_read(&image->file, buf, D64_SECTOR_LEN) == D64_SECTOR_LEN;
}

// "NAME             ID 2A" from the header sector
static void d64_read_diskname(D64_IMAGE *image) {

   uint8_t *s = image->sector_buf;
   const uint8_t *name, *id;

   if (image->type == D64_TYPE_D81) {
      name = s + 0x04;     // name $04-$13, id $16-$17, DOS type $19-$1A
      id = s + 0x16;
   } else {
      name = s + 0x90;     // name $90-$9F, id $A2-$A3, DOS type $A5-$A6
      id = s + 0xA2;
   }

   char *d = image->diskname;
   for (uint8_t i = 0; i < 16; i++)
      *d++ = (name[i] == 0xA0) ? ' ' : name[i];

   *d++ = ' ';
   *d++ = id[0];
   *d++ = id[1];
   *d++ = ' ';
   *d++ = id[3];         // DOS type (after the $A0 separator)
   *d++ = id[4];

   image->diskname_len = d - image->diskname;
}

bool d64_open(D64_IMAGE *image, const char *filename) {

   if (!file_open(&image->file, filename, FA_READ))
      return false;

   FSIZE_t size = f_size(&image->file);
   image->type = d64_get_type(size);
   image->tracks = d64_get_tracks(size);

   uint8_t header_track = (image->type == D64_TYPE_D81) ? D81_TRACK_DIR : D64_TRACK_DIR;

   if (image->type == D64_TYPE_UNKNOWN ||
       !d64_read_sector(image, header_track, 0, image->sector_buf)) {
      file_close(&image->file);
      return false;
   }

   d64_read_diskname(image);

   // the header links to the first directory sector
   image->first_dir_track = image->sector_buf[0];
   image->first_dir_sector = image->sector_buf[1];
   if (image->first_dir_track != header_track) {
      // non standard link: use the default position
      image->first_dir_track = header_track;
      image->first_dir_sector = (image->type == D64_TYPE_D81) ? 3 : 1;
   }

   d64_rewind_dir(image);
   return true;
}

bool d64_close(D64_IMAGE *image) {
   return file_close(&image->file);
}

void d64_rewind_dir(D64_IMAGE *image) {
   image->next_track = image->first_dir_track;
   image->next_sector = image->first_dir_sector;
   image->entry_no = DIR_ENTRIES_SECTOR;     // load a sector at the next read
   image->dir_sectors = 0;
}

// read next used directory entry (scratched entries are skipped)
bool d64_read_dir(D64_IMAGE *image) {

   while (true) {

      if (image->entry_no >= DIR_ENTRIES_SECTOR) {

         if (!image->next_track || image->dir_sectors >= DIR_MAX_SECTORS ||
             !d64_read_sector(image, image->next_track, image->next_sector, image->sector_buf)) {
            d64_rewind_dir(image);
            return false;
         }

         image->dir_sectors++;
         image->next_track = image->sector_buf[0];
         image->next_sector = image->sector_buf[1];
         image->entry_no = 0;
      }

      uint8_t *e = image->sector_buf + image->entry_no * DIR_ENTRY_LEN;
      image->entry_no++;

      if (e[2] == CBM_FILE_DEL)
         continue;

      image->entry.type = e[2];
      image->entry.track = e[3];
      image->entry.sector = e[4];
      memcpy(image->entry.filename, e + 5, 16);
      image->entry.blocks = e[30] | (e[31] << 8);
      return true;
   }
}

// read the file of the last entry as PRG (load address + data), returns size or 0
size_t d64_read_prg(D64_IMAGE *image, uint8_t *buf, size_t buf_size) {

   uint8_t track = image->entry.track;
   uint8_t sector = image->entry.sector;
   uint8_t data[D64_SECTOR_LEN];
   size_t len = 0;

   // a PRG is < 64K: at most 259 blocks (loop guard)
   for (uint16_t blocks = 0; blocks < 260; blocks++) {

      if (!d64_read_sector(image, track, sector, data))
         return 0;

      // last block: data[1] is the index of the last used byte
      uint8_t next_track = data[0];
      size_t n = next_track ? (D64_SECTOR_LEN - 2) : (data[1] >= 2 ? data[1] - 1 : 0);

      if (len + n > buf_size)
         return 0;

      memcpy(buf + len, data + 2, n);
      len += n;

      if (!next_track) {
         // PRG should at least have a 2 byte load address and 1 byte of data
         return (len > 2 && len < 64*1024) ? len : 0;
      }

      track = next_track;
      sector = data[1];
   }

   return 0;
}
