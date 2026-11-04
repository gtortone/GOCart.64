#include <stdio.h>
#include <cstring>

#include "config.h"
#include "filesystem.h"
#include "file_types.h"
#include "cartridge.h"
#include "c64_bus.h"
#include <stdlib.h>

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

// bus timing value (decimal), false if not a number in range
static bool config_bus_value(volatile uint32_t *dest, const char *src, const char *end) {
   char tmp[8];
   if (!config_copy_value(tmp, sizeof(tmp), src, end) || !tmp[0])
      return false;
   char *e;
   unsigned long v = strtoul(tmp, &e, 10);
   if (*e || v > CONFIG_BUS_MAX)
      return false;
   *dest = v;
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
      } else if (strncmp(line, "bus_edge=", 9) == 0) {
         if (!config_bus_value(&c64_bus_edge_cycles, line + 9, end))
            printf("config: bad bus_edge value\n");
      } else if (strncmp(line, "bus_hold=", 9) == 0) {
         if (!config_bus_value(&c64_bus_hold_cycles, line + 9, end))
            printf("config: bad bus_hold value\n");
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

   printf("config: path '%s', file '%s', bus edge %lu, hold %lu cycles\n",
          cfg_file.path, cfg_file.file,
          (unsigned long)c64_bus_edge_cycles, (unsigned long)c64_bus_hold_cycles);
   return path_ok;
}

static bool config_write_str(FIL *file, const char *str) {
   size_t len = strlen(str);
   return file_write(file, (void *)str, len) == len;
}

bool config_save(void) {

   bool bus_default = c64_bus_edge_cycles == C64_BUS_EDGE_DELAY_CYCLES &&
                      c64_bus_hold_cycles == C64_BUS_HOLD_CYCLES;
   if (!cfg_file.path[0] && bus_default)
      return false;

   FIL file;
   if (!file_open(&file, CONFIG_FILE, FA_WRITE|FA_CREATE_ALWAYS))
      return false;

   bool ok = true;
   if (cfg_file.path[0])
      ok = config_write_str(&file, "path=") &&
           config_write_str(&file, cfg_file.path) &&
           config_write_str(&file, "\nfile=") &&
           config_write_str(&file, cfg_file.file) &&
           config_write_str(&file, "\n");
   if (ok && !bus_default) {
      char line[48];
      snprintf(line, sizeof(line), "bus_edge=%lu\nbus_hold=%lu\n",
               (unsigned long)c64_bus_edge_cycles, (unsigned long)c64_bus_hold_cycles);
      ok = config_write_str(&file, line);
   }

   ok = file_close(&file) && ok;

   if (!ok)
      printf("E: failed to save %s\n", CONFIG_FILE);

   return ok;
}
