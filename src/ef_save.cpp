#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "ef_save.h"
#include "cartridge.h"
#include "filesystem.h"

/*
 * EasyFlash save file (see ef_save.h)
 *
 * Changes made by the cartridge through the EAPI (flash writes/erases) are
 * kept in a file next to the CRT: "<name>.crt" -> "<name>.sav". It holds the
 * 8K chunks (bank, ROML/ROMH) that differ from the CRT, so the CRT itself is
 * never modified and deleting the .sav restores the original cartridge.
 *
 *   SAVE_HEADER, then for every chunk: SAVE_RECORD + 8K of data
 *
 * The save file is applied when the CRT is started, if its header matches the
 * CRT (data size and hash). It is written SAVE_DELAY_US after the last EAPI
 * write (the cartridge keeps running), or before another cartridge is started.
 * It is written to a temporary file first and then renamed.
 */
#define SAVE_MAGIC        "GC64SAV"
#define SAVE_VERSION      1
#define SAVE_DELAY_US     (1000 * 1000)
#define SAVE_EXT          ".sav"
#define SAVE_TMP_EXT      ".tmp"

#pragma pack(push)
#pragma pack(1)
typedef struct {
   char magic[8];          // SAVE_MAGIC
   uint16_t version;       // SAVE_VERSION
   uint16_t chunks;        // number of records
   uint32_t crt_size;      // CRT data size (chips)
   uint32_t crt_hash;      // FNV-1a of the CRT data (packed 8K chunks)
} SAVE_HEADER;

typedef struct {
   uint8_t bank;           // EasyFlash bank (0-63)
   uint8_t romh;           // 0: ROML, 1: ROMH
   uint16_t reserved;
} SAVE_RECORD;
#pragma pack(pop)

static char save_path[256 + 64];                  // "" = no save file
static uint32_t save_crt_size;
static uint32_t save_crt_hash;
static uint8_t save_modified[EF_BANKS * 2 / 8];   // chunks different from the CRT
static bool save_pending;                         // changes not written yet
static uint64_t save_last_change;

static inline bool save_is_modified(uint8_t bank, bool romh) {
   uint8_t n = bank * 2 + romh;
   return save_modified[n >> 3] & (1 << (n & 7));
}

// FNV-1a 32 bit
uint32_t ef_save_hash(const uint8_t *data, uint32_t len) {
   uint32_t h = 2166136261u;
   while (len--) {
      h ^= *data++;
      h *= 16777619u;
   }
   return h;
}

// "<dir>/<name>.crt" -> "<dir>/<name>.sav" (absolute: the directory may change)
static bool save_set_path(const char *file_name) {

   save_path[0] = 0;
   size_t max = sizeof(save_path) - sizeof(SAVE_TMP_EXT) - sizeof(SAVE_EXT);

   if (file_name[0] == '/' || strchr(file_name, ':')) {
      if (strlen(file_name) >= max)
         return false;
      strcpy(save_path, file_name);
   } else {
      if (!dir_current(save_path, max))
         return false;
      size_t len = strlen(save_path);
      if (len + 1 + strlen(file_name) >= max) {
         save_path[0] = 0;
         return false;
      }
      if (len && save_path[len - 1] != '/')
         strcat(save_path, "/");
      strcat(save_path, file_name);
   }

   // replace the extension
   char *slash = strrchr(save_path, '/');
   char *dot = strrchr(save_path, '.');
   if (dot && (!slash || dot > slash))
      *dot = 0;
   strcat(save_path, SAVE_EXT);
   return true;
}

// apply the save file to the cartridge data (before core1 starts)
static void save_load(void) {

   FIL file;
   if (!file_open(&file, save_path, FA_READ))
      return;     // no save file yet

   SAVE_HEADER h;
   bool ok = file_read(&file, &h, sizeof(h)) == sizeof(h) &&
             memcmp(h.magic, SAVE_MAGIC, sizeof(h.magic)) == 0 &&
             h.version == SAVE_VERSION;

   if (ok && (h.crt_size != save_crt_size || h.crt_hash != save_crt_hash)) {
      printf("W: %s is for another cartridge, ignored\n", save_path);
      file_close(&file);
      return;
   }

   uint16_t n = 0;
   for (; ok && n < h.chunks; n++) {
      SAVE_RECORD r;
      uint8_t *chunk;
      ok = file_read(&file, &r, sizeof(r)) == sizeof(r) && r.bank < EF_BANKS &&
           (chunk = ef_chunk(r.bank, r.romh, true)) != NULL &&
           file_read(&file, chunk, EF_CHUNK_SIZE) == EF_CHUNK_SIZE;
      if (ok) {
         uint8_t i = r.bank * 2 + (r.romh ? 1 : 0);
         save_modified[i >> 3] |= 1 << (i & 7);
      }
   }

   file_close(&file);

   if (ok)
      printf("EasyFlash: %d chunks loaded from %s\n", n, save_path);
   else
      printf("E: %s not valid\n", save_path);
}

// a chunk was changed by the cartridge
void ef_save_mark(uint8_t bank, bool romh) {
   uint8_t i = bank * 2 + (romh ? 1 : 0);
   save_modified[i >> 3] |= 1 << (i & 7);
   save_pending = true;
   save_last_change = time_us_64();
}

static bool save_write_rec(FIL *file, uint8_t bank, bool romh) {
   SAVE_RECORD r = { bank, (uint8_t)(romh ? 1 : 0), 0 };
   uint8_t *chunk = ef_chunk(bank, romh, false);
   return file_write(file, &r, sizeof(r)) == sizeof(r) &&
          file_write(file, chunk, EF_CHUNK_SIZE) == EF_CHUNK_SIZE;
}

// write all the changed chunks (core1 keeps running: it only reads them)
void ef_save_flush(void) {

   save_pending = false;
   if (!save_path[0])
      return;

   char tmp_path[sizeof(save_path)];
   strcpy(tmp_path, save_path);
   strcat(tmp_path, SAVE_TMP_EXT);

   SAVE_HEADER h;
   memcpy(h.magic, SAVE_MAGIC, sizeof(h.magic));
   h.version = SAVE_VERSION;
   h.chunks = 0;
   h.crt_size = save_crt_size;
   h.crt_hash = save_crt_hash;
   for (uint8_t bank = 0; bank < EF_BANKS; bank++)
      h.chunks += save_is_modified(bank, false) + save_is_modified(bank, true);

   FIL file;
   bool ok = file_open(&file, tmp_path, FA_WRITE | FA_CREATE_ALWAYS) &&
             file_write(&file, &h, sizeof(h)) == sizeof(h);

   for (uint8_t bank = 0; ok && bank < EF_BANKS; bank++) {
      for (uint8_t romh = 0; ok && romh < 2; romh++) {
         if (save_is_modified(bank, romh))
            ok = save_write_rec(&file, bank, romh);
      }
   }

   ok = file_close(&file) && ok;

   // replace the old save file
   if (ok) {
      f_unlink(save_path);
      ok = f_rename(tmp_path, save_path) == FR_OK;
   }

   if (ok)
      printf("EasyFlash: %d chunks saved to %s\n", h.chunks, save_path);
   else
      printf("E: failed to write %s\n", save_path);
}

// write a pending save and forget the current save file
void ef_save_stop(void) {
   if (save_pending)
      ef_save_flush();
   save_path[0] = 0;
   save_pending = false;
   memset(save_modified, 0, sizeof(save_modified));
}

// set up the save file of an EasyFlash cartridge just loaded
void ef_save_start(const char *file_name, uint32_t crt_size, uint32_t crt_hash) {
   save_crt_size = crt_size;
   save_crt_hash = crt_hash;
   if (file_name && save_set_path(file_name))
      save_load();
   else
      printf("W: EasyFlash changes will not be saved\n");
}

// the cartridge stopped writing: time to write the save file
bool ef_save_due(void) {
   return save_pending && time_us_64() - save_last_change > SAVE_DELAY_US;
}
