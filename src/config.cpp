#include <stdio.h>
#include <cstring>

#include "config.h"
#include "filesystem.h"
#include "file_types.h"
#include "cartridge.h"

extern CFG_FILE cfg_file;

#define CONFIG_MAX_SIZE   2048

// copy a value up to end of line, returns false if it does not fit
static bool config_copy_value(char *dest, size_t dest_size, const char *src, const char *end) {

   size_t len = 0;
   while (src + len < end && src[len] != '\n' && src[len] != '\r')
      len++;

   if (len >= dest_size)
      return false;

   memcpy(dest, src, len);
   dest[len] = 0;
   return true;
}

bool config_load(void) {

   FIL file;
   if (!file_open(&file, CONFIG_FILE, FA_READ))
      return false;

   // temporary buffer: KFF_BUF is free before the launcher starts
   char *buf = (char *)KFF_BUF;
   uint32_t len = file_read(&file, buf, CONFIG_MAX_SIZE);
   file_close(&file);

   const char *end = buf + len;
   const char *line = buf;
   bool path_ok = false;

   while (line < end) {

      if (strncmp(line, "path=", 5) == 0) {
         path_ok = config_copy_value(cfg_file.path, sizeof(cfg_file.path), line + 5, end);
         if (!path_ok)
            cfg_file.path[0] = 0;
      } else if (strncmp(line, "file=", 5) == 0) {
         if (!config_copy_value(cfg_file.file, sizeof(cfg_file.file), line + 5, end))
            cfg_file.file[0] = 0;
      }

      // next line
      while (line < end && *line != '\n')
         line++;
      line++;
   }

   if (!path_ok) {
      cfg_file.path[0] = 0;
      cfg_file.file[0] = 0;
   }

   printf("config: path '%s', file '%s'\n", cfg_file.path, cfg_file.file);
   return path_ok;
}

static bool config_write_str(FIL *file, const char *str) {
   size_t len = strlen(str);
   return file_write(file, (void *)str, len) == len;
}

bool config_save(void) {

   if (!cfg_file.path[0])
      return false;

   FIL file;
   if (!file_open(&file, CONFIG_FILE, FA_WRITE|FA_CREATE_ALWAYS))
      return false;

   bool ok = config_write_str(&file, "path=") &&
             config_write_str(&file, cfg_file.path) &&
             config_write_str(&file, "\nfile=") &&
             config_write_str(&file, cfg_file.file) &&
             config_write_str(&file, "\n");

   ok = file_close(&file) && ok;

   if (!ok)
      printf("E: failed to save %s\n", CONFIG_FILE);

   return ok;
}
