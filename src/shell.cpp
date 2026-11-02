#include <stdio.h>
#include <stdlib.h>     // qsort
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"

#include "board.h"
#include "utils.h"
#include "c64_interface.h"
#include "cartridge.h"
#include "filesystem.h"
#include "menu.h"
#include "shell_test.h"

#define CMD_BUFFER_SIZE    128
#define PATH_BUFFER_SIZE   128

static char path[PATH_BUFFER_SIZE];

int cmp_filename(const void *a, const void *b) {
   const FILINFO *fa = (FILINFO *)a;
   const FILINFO *fb = (FILINFO *)b;

   int da = (fa->fattrib & AM_DIR) != 0;
   int db = (fb->fattrib & AM_DIR) != 0;

   if (da != db)
      return db - da;

   const char *na = fa->fname;
   const char *nb = fb->fname;

   return strcmp(na, nb);
}

void print_prompt(bool init=false) {

   if(init)
      printf("\n\n-- GOCart.64 shell --\n\n");

   dir_current(path, PATH_BUFFER_SIZE);
   printf("%s:> ", path);

}

void poll_shell(void) {

   static char cmd_buffer[CMD_BUFFER_SIZE];
   static uint8_t cmd_index = 0;
   static char prev_path[PATH_BUFFER_SIZE];

   static DIR dir;
   static FILINFO fno;
   int c;

   if ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {

      if (c == '\r') {     // cr + lf
         putchar(c);
         putchar('\n');
      } else if (c == '\b') {   // backspace or del
         if (cmd_index > 0) {
            cmd_index--;
            putchar('\b');
            putchar(' ');
            putchar('\b');
         } 
         return;
      } else putchar(c);      // echo
                                       
      // process command
      if (c == '\r' || c == '\n') {
         cmd_buffer[cmd_index] = '\0';
         trim(cmd_buffer);
         
         char *token = strtok(cmd_buffer, " ");

         if (strcmp(token, "reset") == 0) {
            // reset command
            // parameter: [cpu], [c64]
            token = strtok(NULL, " ");
            if (strcmp(token, "c64") == 0) {
               printf("C64 reset\n");
               c64_set_exrom_game(1, 1);
               launcher_disable(); 
               c64_reset();
            } else if (strcmp(token, "pico") == 0) {
               printf("PICO reset\n");
               launcher_disable(); 
               watchdog_enable(100, 0);
            } else {
               printf("reset [c64 | pico]\n");
            }
         } else if (strcmp(token, "menu") == 0) {
            if (!launcher_running()) {
               run_launcher();
               cmd_index = 0;
               printf("%s:> ", path);
               menu_loop();
               return;     // return when launcher exit
            }
         } else if (strcmp(token, "load") == 0) {
            // load file
            // parameter: <filename>
            token = strtok(NULL, "");
            trim(token);
            if(strlen(path) == 1)
               sprintf(prev_path, "%s%s", path, token);
            else
               sprintf(prev_path, "%s/%s", path, token);
            FileReader fr(prev_path);
            if(fr.eof())
               printf("E: file not found (%s)\n", prev_path);
            else
               run_cart(fr, prev_path);
         } else if (strcmp(token, "test") == 0) {
            // launch every supported file of the current directory
            // parameters (any order): [start index] [pattern] [noclean]
            const char *pattern = NULL;
            uint32_t start = 1;
            bool clean = true;
            while ((token = strtok(NULL, " ")) != NULL) {
               char *end;
               unsigned long n = strtoul(token, &end, 10);
               if (*token && !*end)
                  start = n;
               else if (strcmp(token, "noclean") == 0)
                  clean = false;
               else
                  pattern = token;
            }
            shell_test(pattern, start, clean);
         } else if (strcmp(token, "cputrace") == 0) {
            // EasyFlash CPU trace (EF_DEBUG_CPU_TRACE in cartridge.cpp)
            cpu_trace_shell(strtok(NULL, ""));
         } else if (strcmp(token, "clean") == 0) {
            // power-on like C64 state (RAM, VIC-II, SID cleared)
            // parameter: [on | off]  enable/disable it before every launch,
            //            no parameter: run it now (C64 kept in reset)
            token = strtok(NULL, " ");
            if (token && strcmp(token, "on") == 0) {
               c64_clean_enable(true);
            } else if (token && strcmp(token, "off") == 0) {
               c64_clean_enable(false);
            } else if (token) {
               printf("clean [on | off]\n");
            } else {
               printf("C64 clean start: %s\n", run_c64_clean() ? "done" : "failed");
            }
            printf("clean start before every launch: %s\n", c64_clean_is_enabled() ? "on" : "off");
         } else if (strcmp(token, "lss") == 0) {
            // list files/directories
            FILINFO *file_list = (FILINFO *)(crt_buf + 3*16*1024); // start from CRT_BANK(2)
            dir_open(&dir, "*");
            int fnum=0;
            while(dir_read(&dir, &fno)) {
               if(fno.fname[0] == 0)
                  break;
               memcpy(&file_list[fnum], &fno, sizeof(FILINFO));
               fnum++;
            }
            dir_close(&dir);
            qsort(file_list, fnum, sizeof(FILINFO), cmp_filename);
            for(int i=0; i<fnum; i++) {
               fno = file_list[i];
               if (fno.fattrib & AM_DIR)
                  printf("[DIR] %s\n", fno.fname);
               else
                  printf("[FILE] %s (%llu bytes)\n", fno.fname, fno.fsize);
            }
         } else if (strcmp(token, "ls") == 0) {
            // list files/directories
            token = strtok(NULL, " ");
            dir_open(&dir, token);
            while(dir_read(&dir, &fno)) {
               if(fno.fname[0] == 0)
                  break;
               char *name = *fno.fname ? fno.fname : fno.fname;
               if (fno.fattrib & AM_DIR)
                  printf("[DIR] %s\n", name);
               else
                  printf("[FILE] %s (%llu bytes)\n", name, fno.fsize);
            }
            dir_close(&dir);
         } else if (strcmp(token, "cd") == 0) {
            token = strtok(NULL, " ");
            strcpy(prev_path, path);
            if(token[0] == '/') {    // absolute path
               sprintf(path, "%s", token);
            } else {
               if (strcmp(token, ".") == 0) {
                  ;  // do nothing
               } else if (strcmp(token, "..") == 0) {
                  char *last_slash = strrchr(path, '/');
                  if (last_slash != NULL) {
                     *(last_slash) = '\0';
                     if(strlen(path) == 2)   // e.g. "0:"
                        strcpy(path, "/");
                  }
               } else {
                  if(strlen(path) == 1)
                     sprintf(path, "%s%s", prev_path, token); // relative path  (first level)
                  else
                     sprintf(path, "%s/%s", prev_path, token); // relative path
               }
            } 
            if(!dir_change(path)) {
               printf("E: directory %s not found\n", token);
               strcpy(path, prev_path);
            }
         } else if (strcmp(token, "info") == 0) {
            extern char __data_start__;
            extern char __data_end__;
            extern char __bss_start__;
            extern char __bss_end__;
            extern char __StackLimit;
            extern char __StackTop;
            uint32_t data_size = &__data_end__ - &__data_start__;
            uint32_t bss_size  = &__bss_end__ - &__bss_start__;
            uint32_t stack_size = &__StackTop - &__StackLimit;
            printf("DATA:  %lu bytes\n", data_size);
            printf("BSS:   %lu bytes\n", bss_size);
            printf("STACK: %lu bytes reserved\n", stack_size);
            printf("CPU frequency: %.0f MHz\n", clock_get_hz(clk_sys) / 1e6);
            printf("flash size: %d bytes\n", PICO_FLASH_SIZE_BYTES);
            printf("flash sector size: %d bytes\n", FLASH_SECTOR_SIZE);
            printf("flash page size: %d bytes\n", FLASH_PAGE_SIZE);
            printf("flash area address: 0x%X\n", FLASH_AREA_OFFSET);
            printf("XIP_BASE address: 0x%X\n", XIP_BASE);
            extern uint8_t crt_buf[CRT_BUFFER_SIZE];
            printf("max CRT size: %d bytes\n", sizeof(crt_buf));
         } else if (strlen(cmd_buffer) != 0){
            printf("%s: unknown command\n", cmd_buffer);
         }
         cmd_index = 0;
         print_prompt();

      } else if (cmd_index < CMD_BUFFER_SIZE - 1) {
         cmd_buffer[cmd_index++] = c;
      }
   }
}


