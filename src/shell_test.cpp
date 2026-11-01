#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "shell_test.h"
#include "cartridge.h"
#include "filesystem.h"
#include "file_types.h"
#include "menu_sd.h"
#include "menu_image.h"
#include "FileReader.h"

typedef enum {
   TEST_OK = 0,
   TEST_FAIL,
   TEST_LOAD_ERROR,
   TEST_SKIP,
   TEST_TOO_LARGE,
   TEST_QUIT,
   TEST_RETRY,
   TEST_NUM_RESULTS
} TEST_RESULT;

static const char *test_result_names[TEST_NUM_RESULTS] = {
   "OK", "FAIL", "LOAD ERROR", "SKIP", "TOO LARGE", "QUIT", "RETRY"
};

// shared work areas (never used at the same time): the core0 stack is small
// and the heap is limited
static FILINFO test_info;
static FIL test_file;

static bool test_supported(FILINFO *info) {

   switch (get_file_type(info)) {
      case FILE_CRT:
      case FILE_ROM:
      case FILE_PRG:
      case FILE_P00:
      case FILE_T64:
      case FILE_D64:
         return true;
   }
   return false;
}

/*
 * Find the first supported file whose name is greater than prev (alphabetical
 * order, case sensitive like the shell "lss" command). No file list is kept in
 * memory: crt_buf is overwritten by every launch.
 * Returns false when there are no more files.
 */
static bool test_next_file(const char *pattern, const char *prev, char *next, size_t next_size) {

   DIR dir;
   FILINFO *info = &test_info;
   bool found = false;

   if (!dir_open(&dir, pattern))
      return false;

   while (dir_read(&dir, info) && info->fname[0]) {

      if (info->fattrib & AM_DIR || !test_supported(info))
         continue;

      if (strcmp(info->fname, prev) <= 0)
         continue;

      if (!found || strcmp(info->fname, next) < 0) {
         size_t len = strnlen(info->fname, sizeof(info->fname));
         if (len >= next_size)
            len = next_size - 1;
         memcpy(next, info->fname, len);
         next[len] = 0;
         found = true;
      }
   }

   dir_close(&dir);
   return found;
}

static uint32_t test_count_files(const char *pattern) {

   DIR dir;
   FILINFO *info = &test_info;
   uint32_t count = 0;

   if (!dir_open(&dir, pattern))
      return 0;

   while (dir_read(&dir, info) && info->fname[0]) {
      if (!(info->fattrib & AM_DIR) && test_supported(info))
         count++;
   }

   dir_close(&dir);
   return count;
}

/*
 * Largest file that fits in the cartridge memory:
 *  - CRT: crt_buf plus the CRT header (64 bytes) and one 16 byte chip header
 *    for every 8K of data
 *  - ROM/BIN: 16K (see run_rom)
 */
#define TEST_CRT_MAX_FILE_SIZE   (64 + CRT_BUFFER_SIZE + 16 * (CRT_BUFFER_SIZE / (8*1024)))
#define TEST_ROM_MAX_FILE_SIZE   (16*1024)

static bool test_too_large(FILINFO *info) {

   switch (get_file_type(info)) {
      case FILE_CRT:
         return info->fsize > TEST_CRT_MAX_FILE_SIZE;
      case FILE_ROM:
         return info->fsize > TEST_ROM_MAX_FILE_SIZE;
   }
   return false;
}

// load the first PRG of a T64/D64 image in PRG_BUF, returns its size or 0
static uint32_t test_load_image_prg(const char *file_name, uint8_t file_type, char *name) {

   uint32_t size = 0;

   if (file_type == FILE_T64) {
      T64_IMAGE *image = &image_state.t64.image;
      if (!t64_open(image, file_name))
         return 0;
      if (t64_read_dir(image)) {
         image_copy_name(name, image->entry.filename);
         size = t64_read_prg(image, PRG_BUF, PRG_BUF_SIZE);
      }
      t64_close(image);

   } else {
      D64_IMAGE *image = &image_state.d64.image;
      if (!d64_open(image, file_name))
         return 0;
      while (d64_read_dir(image)) {
         if ((image->entry.type & 0x07) == CBM_FILE_PRG) {
            image_copy_name(name, image->entry.filename);
            size = d64_read_prg(image, PRG_BUF, PRG_BUF_SIZE);
            break;
         }
      }
      d64_close(image);
   }

   return size;
}

// launch a file, returns false on load error
static bool test_launch(const char *file_name) {

   if (!file_stat(file_name, &test_info))
      return false;

   uint8_t file_type = get_file_type(&test_info);
   char name[33];

   switch (file_type) {

      case FILE_CRT: {
         FileReader fr(file_name);
         return run_cart(fr, file_name) == 0;
      }

      case FILE_ROM: {
         if (!file_open(&test_file, file_name, FA_READ))
            return false;
         memset(PRG_BUF, 0xFF, 16*1024 + 1);
         uint32_t len = file_read(&test_file, PRG_BUF, 16*1024 + 1);
         file_close(&test_file);
         return run_rom(PRG_BUF, len) == ROM_OK;
      }

      case FILE_PRG:
      case FILE_P00: {
         uint32_t size = sd_load_prg(file_name, file_type, name, sizeof(name));
         return size && run_prg(PRG_BUF, size, name);
      }

      case FILE_T64:
      case FILE_D64: {
         name[0] = 0;
         uint32_t size = test_load_image_prg(file_name, file_type, name);
         return size && run_prg(PRG_BUF, size, name);
      }
   }

   return false;
}

// wait for the answer, keeping the running cartridge served (EAPI)
static TEST_RESULT test_ask(void) {

   printf("started? [y]es [n]o [r]etry [s]kip [q]uit: ");

   while (true) {
      cart_poll();

      int c = getchar_timeout_us(0);
      if (c == PICO_ERROR_TIMEOUT)
         continue;

      TEST_RESULT res;
      switch (c) {
         case 'y': case 'Y': res = TEST_OK;    break;
         case 'n': case 'N': res = TEST_FAIL;  break;
         case 'r': case 'R': res = TEST_RETRY; break;
         case 's': case 'S': res = TEST_SKIP;  break;
         case 'q': case 'Q': res = TEST_QUIT;  break;
         default: continue;
      }

      printf("%c\n", c);
      return res;
   }
}

// append "<first>\t<second>\n" to the log file
static void test_log_line(const char *first, const char *second) {

   FIL *file = &test_file;
   if (!file_open(file, TEST_LOG_FILE, FA_WRITE | FA_OPEN_APPEND))
      return;

   file_write(file, (void *)first, strlen(first));
   file_write(file, (void *)"\t", 1);
   file_write(file, (void *)second, strlen(second));
   file_write(file, (void *)"\n", 1);
   file_close(file);
}

static void test_log(const char *file_name, TEST_RESULT res) {
   test_log_line(test_result_names[res], file_name);
}

void shell_test(const char *pattern, uint32_t start, bool clean) {

   // file names are kept in static buffers: the core0 stack is small
   static char current[sizeof(test_info.fname)];
   static char next[sizeof(test_info.fname)];
   uint32_t counts[TEST_NUM_RESULTS] = {};

   if (!pattern || !pattern[0])
      pattern = "*";
   if (!start)
      start = 1;

   uint32_t total = test_count_files(pattern);
   if (!total) {
      printf("test: no supported files (%s)\n", pattern);
      return;
   }

   if (start > total) {
      printf("test: start index %lu, only %lu files\n", (unsigned long)start, (unsigned long)total);
      return;
   }

   printf("test: %lu files, from %lu, %s, results in %s\n", (unsigned long)total,
          (unsigned long)start, clean ? "C64 clean start" : "no clean start", TEST_LOG_FILE);
   test_log_line("---", pattern);     // start of a test run

   // the C64 clean start is done by every launch (run_cart, run_rom, run_prg):
   // "noclean" disables it for the duration of the test
   bool clean_was_enabled = c64_clean_is_enabled();
   c64_clean_enable(clean);

   current[0] = 0;
   uint32_t index = 0;

   while (test_next_file(pattern, current, next, sizeof(next))) {

      strcpy(current, next);
      index++;

      // resume from a given index: same numbering as a complete run
      if (index < start)
         continue;

      printf("\n[%lu/%lu] %s\n", (unsigned long)index, (unsigned long)total, current);

      // files that do not fit in memory are not launched
      if (file_stat(current, &test_info) && test_too_large(&test_info)) {
         printf("too large (%lu bytes), skipped\n", (unsigned long)test_info.fsize);
         counts[TEST_TOO_LARGE]++;
         test_log(current, TEST_TOO_LARGE);
         continue;
      }

      TEST_RESULT res;
      bool retry = false;
      do {
         if (retry)
            printf("\n[%lu/%lu] %s\n", (unsigned long)index, (unsigned long)total, current);
         retry = true;

         if (!test_launch(current)) {
            printf("load error\n");
            res = TEST_LOAD_ERROR;
            break;
         }

         res = test_ask();
      } while (res == TEST_RETRY);

      if (res == TEST_QUIT) {
         printf("resume with: test %lu%s%s%s\n", (unsigned long)index,
                strcmp(pattern, "*") ? " " : "", strcmp(pattern, "*") ? pattern : "",
                clean ? "" : " noclean");
         break;
      }

      counts[res]++;
      test_log(current, res);
   }

   c64_clean_enable(clean_was_enabled);

   printf("\ntest: %lu OK, %lu FAIL, %lu LOAD ERROR, %lu SKIP, %lu TOO LARGE (%lu files)\n",
          (unsigned long)counts[TEST_OK], (unsigned long)counts[TEST_FAIL],
          (unsigned long)counts[TEST_LOAD_ERROR], (unsigned long)counts[TEST_SKIP],
          (unsigned long)counts[TEST_TOO_LARGE], (unsigned long)total);
}
