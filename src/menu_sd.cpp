/*
 * Copyright (c) 2019-2025 Kim Jørgensen
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
 */

#include <stdio.h>
#include <cstdint>

#include "menu_sd.h"
#include "menu.h"
#include "filesystem.h"
#include "file_types.h"

#include "FileReader.h"
#include "cartridge.h"
#include "menu_t64.h"
#include "menu_d64.h"
#include "config.h"

//
SD_STATE sd_state;
extern CFG_FILE cfg_file;
extern const MENU *menu;
//

void sd_format_size(char *buffer, uint32_t size) {
   char units[] = "BKmg";
   char *unit = units;

   while (size >= 1000) {
      size += 24;
      size /= 1024;
      unit++;
   }

   if (size < 100) {
      *buffer++ = ' ';
   }

   if (size < 10) {
      *buffer++ = ' ';
   }

   sprintf(buffer, " %lu%c", size, *unit);
}

void sd_format_element(char *buffer, FILINFO *info) {
   *buffer++ = ' ';

   // Element name
   const uint8_t filename_len = (ELEMENT_LENGTH-1)-5;
   to_petscii_pad(buffer, info->fname, filename_len);
   buffer += filename_len;

   // Element type or size
   if (info->fattrib & AM_DIR)
      sprintf(buffer, "  dir");
   else
      sd_format_size(buffer, info->fsize);
}

void sd_send_not_found(SD_STATE *state) {
   to_petscii_pad(scratch_buf, " no files found", ELEMENT_LENGTH);
   if (state->search[0])
      scratch_buf[0] = SELECTED_ELEMENT;

   c64_send_data(scratch_buf, ELEMENT_LENGTH);
}

uint8_t sd_send_page(SD_STATE *state, uint8_t selected_element) {
   bool send_dot_dot = !state->in_root && state->page_no == 0;
   bool send_not_found = state->page_no == 0;

   FILINFO file_info;
   uint8_t element;
   for (element=0; element<MAX_ELEMENTS_PAGE; element++) {
      if (send_dot_dot) {
         file_info.fname[0] = '.';
         file_info.fname[1] = '.';
         file_info.fname[2] = 0;
         file_info.fattrib = AM_DIR;
         send_dot_dot = false;

      } else {
         if (!dir_read(&state->end_page, &file_info))
            file_info.fname[0] = 0;

         if (file_info.fname[0]) {
            send_not_found = false;
         } else {
            // End of dir
            dir_close(&state->end_page);
            state->dir_end = true;

            if (send_not_found)
               sd_send_not_found(state);

            send_page_end();
            break;
         }
      }

      sd_format_element(scratch_buf, &file_info);
      if (element == selected_element)
         scratch_buf[0] = SELECTED_ELEMENT;

      c64_send_data(scratch_buf, ELEMENT_LENGTH);
   }

   return element;
}

void sd_dir_open(SD_STATE *state) {
   // Append star to end of search string
   size_t search_len = strlen(state->search);

   if (search_len && state->search[search_len-1] != '*') {
      state->search[search_len] = '*';
      state->search[search_len + 1] = 0;
   }

   if (!dir_open(&state->start_page, state->search))
      fail_to_read_sd();

   state->end_page = state->start_page;
   state->page_no = 0;
   state->dir_end = false;
}

void sd_send_prg_message(const char *message) {
   c64_send_prg_message(message);
   //c64_interface(false);      // FIXME
}

void sd_send_warning_restart(const char *message, const char *filename) {
   c64_interface_sync();
   sprintf(scratch_buf, "%s\r\n\r\n%s", message, filename);
   c64_send_warning(scratch_buf);
   //restart_to_menu();   // FIXME
}

uint8_t sd_parse_file_number(char *filename, uint8_t *extension) {
   uint8_t pos = *extension-1;
   if (pos <= 5 || filename[pos--] != ')')
      return 0;

   uint8_t number;
   if (filename[pos] >= '0' && filename[pos] <= '9') {
      number = filename[pos--] - '0';
   } else {
      return 0;
   }

   if (filename[pos] >= '0' && filename[pos] <= '9') {
      number += (filename[pos--] - '0') * 10;
   }

   if (filename[pos--] != '(' || filename[pos] != ' ') {
      return 0;
   }

   *extension = pos;
   return number;
}

/*
bool sd_generate_new_filename(void) {
   char *filename = cfg_file.file;

   uint8_t extension;
   uint8_t length = get_filename_length(filename, &extension);
   if (length <= extension)
      return false;

   uint8_t file_number = sd_parse_file_number(filename, &extension);
   if (++file_number >= 100)
      return false;

   if (length < (sizeof(cfg_file.file)-5)) {
      sprintf(filename + extension, " (%u).crt", file_number);
      return true;
   }

   return false;
}
 
void sd_handle_save_updated_crt(uint8_t flags) {
   sd_send_prg_message("Saving CRT file.");
   if (!load_cfg())
      restart_to_menu();

   if (!(flags & SELECT_FLAG_OVERWRITE)) {
      bool file_exists = true;
      while (file_exists) {
         if (!sd_generate_new_filename()) {
            if (!load_cfg())
               restart_to_menu();
            sd_send_warning_restart("Failed to generate new filename for", cfg_file.file);
         }

         FILINFO file_info;
         file_exists = file_stat(cfg_file.file, &file_info);
      }
   }

   uint8_t banks = 0;
   FIL file;

   if (!file_open(&file, cfg_file.file, FA_WRITE|FA_CREATE_ALWAYS) ||
     !crt_write_header(&file, CRT_EASYFLASH, 1, 0, "EASYFLASH") ||
     !(banks = crt_write_file(&file))) {
     sd_send_warning_restart("Failed to write CRT file", cfg_file.file);
   }

   file_close(&file);

   crt_buf_valid(banks);
   save_cfg();
   restart_to_menu();
}

bool sd_crt_updated(SD_STATE *state) {
   if (cfg_file.boot_type != CFG_CRT ||
     cfg_file.crt.type != CRT_EASYFLASH ||
     !cfg_file.file[0] ||
     !crt_buf_is_updated() ||
     state->ignore_crt_updated) {
        return false;
   }

   state->ignore_crt_updated = true;   // Don't ask to save file again
   return true;
}
*/

uint8_t sd_handle_dir(SD_STATE *state) {
   //if (sd_crt_updated(state))
   //   return handle_unsaved_crt(cfg_file.file, sd_handle_save_updated_crt);
   
   sd_dir_open(state);

   dir_current(cfg_file.path, sizeof(cfg_file.path));
   state->in_root = format_path(scratch_buf, false);
   scratch_buf[0] = state->search[0] ? SEARCH_SUPPORTED : CLEAR_SEARCH;

   FILINFO file_info;
   bool found = false;
   uint8_t selected_element = MAX_ELEMENTS_PAGE;

   // *** Search for last selected element ***
   if (cfg_file.file[0]) {
      DIR first_page = state->start_page;
      while (true) {
         uint8_t element = 0;
         if (!state->in_root && state->page_no == 0)
            element++;

         for (; element<MAX_ELEMENTS_PAGE; element++) {
            if (!dir_read(&state->end_page, &file_info))
               file_info.fname[0] = 0;

            if (!file_info.fname[0])
               break;

            if (strncmp(cfg_file.file, file_info.fname, sizeof(cfg_file.file)) == 0) {
               found = true;
               break;
            }
         }

         if (found) {
            state->end_page = state->start_page;
            selected_element = element;
            break;
         }

         if (!file_info.fname[0]) {
            state->start_page = first_page;
            state->end_page = first_page;
            state->page_no = 0;
            break;
         }

         state->start_page = state->end_page;
         state->page_no++;
      }
   }

   if (!found) {
      cfg_file.file[0] = 0;

   } /*else if (cfg_file.img.element != ELEMENT_NOT_SELECTED) {

      uint8_t file_type = get_file_type(&file_info);
      if (file_type == FILE_D64) {
         menu = d64_menu_init(file_info.fname);
         return menu->dir(menu->state);
      } else if (file_type == FILE_T64) {
         menu = t64_menu_init(file_info.fname);
         return menu->dir(menu->state);
   }
      }*/

   c64_send_data(scratch_buf, DIR_NAME_LENGTH);
   sd_send_page(state, selected_element);
   return CMD_READ_DIR;
}

uint8_t sd_handle_change_dir(SD_STATE *state, char *path, bool select_old) {
   if (select_old && !state->in_root) {
      uint16_t dir_index = 0;
      uint16_t len;
      for (len = 0; len < sizeof(cfg_file.path); len++) {
         char c = cfg_file.path[len];
         if (!c)
            break;
         if (c == '/')
            dir_index = len+1;
      }

      memcpy(cfg_file.file, cfg_file.path + dir_index, len - dir_index);
      cfg_file.file[len - dir_index] = 0;

   } else {
      cfg_file.file[0] = 0;
   }

   state->search[0] = 0;
   if (!dir_change(path))
      fail_to_read_sd();

   return sd_handle_dir(state);
}

uint8_t sd_handle_dir_next_page(SD_STATE *state) {
    if (!state->dir_end) {
      DIR start = state->end_page;
      state->page_no++;

      if (sd_send_page(state, MAX_ELEMENTS_PAGE) > 0) {
         state->start_page = start;
      } else {
         state->page_no--;
      }

    } else {
        send_page_end();
    }

    return CMD_READ_DIR_PAGE;
}

uint8_t sd_handle_dir_prev_page(SD_STATE *state) {
   if (state->page_no) {
      uint16_t target_page = state->page_no-1;
      bool not_found = false;

      sd_dir_open(state);

      uint16_t elements_to_skip = MAX_ELEMENTS_PAGE * target_page;
      if (elements_to_skip && !state->in_root) elements_to_skip--;

      while (elements_to_skip--) {
         FILINFO file_info;
         if (!dir_read(&state->end_page, &file_info))
            file_info.fname[0] = 0;

         if (!file_info.fname[0]) {
            not_found = true;
            break;
         }
      }

      if (not_found) {
         state->end_page = state->start_page;
      } else {
         state->page_no = target_page;
         state->start_page = state->end_page;
      }

      sd_send_page(state, MAX_ELEMENTS_PAGE);
      return CMD_READ_DIR_PAGE;
   }

   return handle_page_end();
}

/*
uint8_t sd_handle_delete_file(const char *file_name) {
   sd_send_prg_message("Deleting file.");

   if (!file_delete(file_name))
      sd_send_warning_restart("Failed to delete file", file_name);

   c64_interface_sync();
   return CMD_MENU;
}

void sd_file_open(FIL *file, const char *file_name) {
   if (!file_open(file, file_name, FA_READ))
      fail_to_read_sd();
}

uint8_t sd_handle_crt_unsupported(uint32_t cartridge_type) {
   sprintf(scratch_buf, "Unsupported %s CRT type (%u)",
        cartridge_type & CRT_C128_CARTRIDGE ? "C128" : "C64",
        cartridge_type & (CRT_C128_CARTRIDGE-1));
   return handle_unsupported_ex("Unsupported", scratch_buf, cfg_file.file);
}

bool sd_c128_only_warning(uint8_t flags) {
   // Warning on a C64
   if (!(flags & SELECT_FLAG_C128) && !(flags & SELECT_FLAG_ACCEPT))
      return true;
   return false;
}

uint8_t sd_handle_c128_only_warning(const char *file_name, uint8_t element) {
   return handle_unsupported_warning("Cartridge will only work on a C128",
      file_name, element);
}
*/

/*
 * Load a PRG or P00 file in PRG_BUF and get the name to display while loading.
 * Returns the PRG size (load address + data) or 0 on error.
 */
uint32_t sd_load_prg(const char *file_name, uint8_t file_type, char *name, size_t name_size) {

   FIL file;
   if (!file_open(&file, file_name, FA_READ))
      return 0;

   size_t name_len = 0;

   if (file_type == FILE_P00) {
      P00_HEADER header;
      if (file_read(&file, &header, sizeof(header)) != sizeof(header) ||
          memcmp(header.signature, "C64File", 7) != 0) {
         file_close(&file);
         return 0;
      }
      // C64 file name from the header
      for (; name_len < 16 && name_len < name_size - 1 && header.filename[name_len]; name_len++)
         name[name_len] = sanitize_char(header.filename[name_len]);
   } else {
      // SD file name, limited to one line on the screen
      for (; name_len < 32 && name_len < name_size - 1 && file_name[name_len]; name_len++) {
         char c = sanitize_char(file_name[name_len]);
         name[name_len] = (c >= 'a' && c <= 'z') ? c - 0x20 : c;
      }
   }
   name[name_len] = 0;

   uint32_t size = file_read(&file, PRG_BUF, PRG_BUF_SIZE);
   file_close(&file);

   // PRG should at least have a 2 byte load address and 1 byte of data
   if (size <= 2 || size >= PRG_BUF_SIZE)
      return 0;

   return size;
}

uint8_t sd_handle_load(SD_STATE *state, const char *file_name, uint8_t file_type, uint8_t flags, uint8_t element) {
   switch (file_type) {

      case FILE_CRT: {
         //FIL file;
         FileReader fr(file_name);
         if (run_cart(fr, file_name) == 0)
            config_save();      // start from here at the next power on
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_PRG:
      case FILE_P00: {
         char name[33];
         uint32_t size = sd_load_prg(file_name, file_type, name, sizeof(name));
         if (!size)
            return handle_unsupported(file_name);

         // the launcher is restarted in EF3 mode: from here on the menu
         // protocol is no longer available, a failed transfer is only logged
         if (run_prg(PRG_BUF, size, name))
            config_save();      // start from here at the next power on
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_ROM: {
         // load in PRG_BUF padded with $FF, read one byte more than 16K to detect larger files
         FIL file;
         if (!file_open(&file, file_name, FA_READ))
            return handle_unsupported(file_name);
         memset(PRG_BUF, 0xFF, 16*1024 + 1);
         uint32_t len = file_read(&file, PRG_BUF, 16*1024 + 1);
         file_close(&file);

         uint8_t res = run_rom(PRG_BUF, len);
         if (res == ROM_C128)
            return handle_unsupported_ex("Unsupported", "C128 cartridges are not supported", file_name);
         if (res != ROM_OK)
            return handle_unsupported(file_name);

         config_save();         // start from here at the next power on
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_D64: {
         const MENU *d64 = d64_menu_init(file_name);
         if (!d64)
            return handle_unsupported(file_name);

         state->search[0] = 0;
         cfg_file.img.element = 0;
         menu = d64;
         return menu->dir(menu->state);
      }
      break;

      case FILE_T64: {
         const MENU *t64 = t64_menu_init(file_name);
         if (!t64)
            return handle_unsupported(file_name);

         state->search[0] = 0;
         cfg_file.img.element = 0;
         menu = t64;
         return menu->dir(menu->state);
      }
      break;
      /*
      case FILE_PRG:
      case FILE_P00: {
         cfg_file.img.mode =
             file_type == FILE_P00 ? PRG_MODE_P00 : PRG_MODE_PRG;
         cfg_file.boot_type = CFG_PRG;
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_ROM: {
         FIL file;
         sd_file_open(&file, file_name);
         crt_buf_invalidate();

         uint16_t len = rom_load_file(&file);
         if (len &&  // Look for C128 CRT identifier
             (memcmp("CBM", &crt_buf[0x0007], 3) == 0 ||
              memcmp("CBM", &crt_buf[0x4007], 3) == 0)) {
            cfg_file.crt.type = CRT_C128_NORMAL_CARTRIDGE;
            cfg_file.crt.exrom = 1;
            cfg_file.crt.game = 1;

            if (!crt_is_supported(cfg_file.crt.type))
               return sd_handle_crt_unsupported(cfg_file.crt.type);

            if (sd_c128_only_warning(flags))
               return sd_handle_c128_only_warning(file_name, element);

         } else if (len && len <= 16*1024) {
            cfg_file.crt.type = CRT_NORMAL_CARTRIDGE;
            cfg_file.crt.exrom = 0;
            cfg_file.crt.game = 0;
            // Look for C64 CRT identifier "CBM80"
            if (memcmp("\xc3""\xc2""\xcd""80", &crt_buf[0x0004], 5) == 0) {
               if (len <= 8*1024)
                  cfg_file.crt.game = 1;

             } else {    // Assume Ultimax
               cfg_file.crt.exrom = 1;
               rom_mirror(len);

               // Validate reset vector
               uint8_t reset_hi = crt_buf[0x3ffd];
               if (reset_hi < 0x80 || (reset_hi >= 0xa0 && reset_hi < 0xe0))
                  return handle_unsupported(file_name);
             }
         } else {
            return handle_unsupported(file_name);
         }

         uint8_t banks = ((len - 1) / 16*1024) + 1;
         crt_buf_valid(banks);
         cfg_file.crt.hw_rev = 0;
         cfg_file.crt.flags = CRT_FLAG_VIC | CRT_FLAG_ROM;
         cfg_file.boot_type = CFG_CRT;
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_CRT: {
         FIL file;
         sd_file_open(&file, file_name);

         CRT_HEADER header;
         if (!crt_load_header(&file, &header))
            return handle_unsupported(file_name);

         if (!crt_is_supported(header.cartridge_type))
            return sd_handle_crt_unsupported(header.cartridge_type);

         if (header.cartridge_type & CRT_C128_CARTRIDGE && sd_c128_only_warning(flags))
            return sd_handle_c128_only_warning(file_name, element);

         sd_send_prg_message("Loading CRT file.");
         crt_buf_invalidate();

         uint8_t banks = crt_load_file(&file, header.cartridge_type);
         if (!banks)
            sd_send_warning_restart("Failed to read CRT file", file_name);
         crt_install_eapi(header.cartridge_type);

         crt_buf_valid(banks);
         uint8_t crt_flags = flags & SELECT_FLAG_VIC ? CRT_FLAG_VIC : CRT_FLAG_NONE;
         cfg_file.crt.type = header.cartridge_type;
         cfg_file.crt.hw_rev = header.hardware_revision;
         cfg_file.crt.exrom = header.exrom;
         cfg_file.crt.game = header.game;
         cfg_file.crt.flags = crt_flags;
         cfg_file.boot_type = CFG_CRT;
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_D64: {
         if (flags & SELECT_FLAG_MOUNT) {
            cfg_file.img.mode = DISK_MODE_D64;
            cfg_file.boot_type = CFG_DISK;
            return CMD_WAIT_SYNC;
         }

         if (!(flags & SELECT_FLAG_ACCEPT) && autostart_d64()) {
            cfg_file.img.mode = DISK_MODE_D64;
            cfg_file.boot_type = CFG_DISK;
            return CMD_WAIT_SYNC;
         }

         state->search[0] = 0;
         cfg_file.img.element = 0;
         menu = d64_menu_init(file_name);
         return menu->dir(menu->state);
      }
      break;

      case FILE_T64: {
         if (!(flags & SELECT_FLAG_ACCEPT) && autostart_d64())
            return t64_load_first(file_name);

         state->search[0] = 0;
         cfg_file.img.element = 0;
         menu = t64_menu_init(file_name);
         return menu->dir(menu->state);
      }
      break;

      case FILE_TXT: {
         cfg_file.boot_type = CFG_TXT;
         return CMD_WAIT_SYNC;
      }
      break;

      case FILE_UPD: {
         FIL file;
         sd_file_open(&file, file_name);

         char firmware[FW_NAME_SIZE];
         if (!(flags & SELECT_FLAG_ACCEPT)) {
            if (!upd_load(&file, firmware))
               return handle_unsupported(file_name);

            return handle_upgrade_menu(firmware, element);
         }

         sd_send_prg_message("Upgrading firmware.");
         if (upd_load(&file, firmware)) {
            upd_program();
            save_cfg();
         }
         restart_to_menu();
      }
      break;
      */

      default: {
         return handle_unsupported(file_name);
      }
      break;
   } // end switch
}

uint8_t sd_handle_select(SD_STATE *state, uint8_t flags, uint8_t element) {

   uint8_t element_no = element;

   //cfg_file.boot_type = CFG_NONE;
   //cfg_file.img.element = ELEMENT_NOT_SELECTED;    // don't auto open T64/D64
   cfg_file.file[0] = 0;

   if (!state->in_root && state->page_no == 0) {
      if (element_no == 0) {
         //if (flags & SELECT_FLAG_OPTIONS)
         //    return handle_file_options("..", FILE_DIR_UP, element);

         return sd_handle_change_dir(state, (char *)"..", true);
      }

      element_no--;
   }

   FILINFO file_info;
   DIR dir = state->start_page;
   for (uint8_t i=0; i<=element_no; i++) {
      if (!dir_read(&dir, &file_info))
         fail_to_read_sd();

      if (!file_info.fname[0]) // End of dir
         break;
   }

   dir_close(&dir);

   if (file_info.fname[0] == 0) {
      // File not not found
      state->search[0] = 0;
      return sd_handle_dir(state);
   }

   uint8_t file_type = get_file_type(&file_info);
   strcpy(cfg_file.file, file_info.fname);

   /*
   if (flags & SELECT_FLAG_OPTIONS)
      return handle_file_options(file_info.fname, file_type, element);
   */

   if (file_type == FILE_DIR) {
      //if (!(flags & SELECT_FLAG_MOUNT))
         return sd_handle_change_dir(state, file_info.fname, false);

      //cfg_file.img.mode = DISK_MODE_FS;
      //cfg_file.boot_type = CFG_DISK;
      //return CMD_WAIT_SYNC;
   }

   /*
   if (flags & SELECT_FLAG_DELETE)
      return sd_handle_delete_file(file_info.fname);

   if (!(flags & SELECT_FLAG_MOUNT) && file_type == FILE_PRG) {
      cfg_file.img.mode = DISK_MODE_FS;
      cfg_file.boot_type = CFG_DISK;
      return CMD_WAIT_SYNC;
   }
   */

   return sd_handle_load(state, file_info.fname, file_type, flags, element);
}

uint8_t sd_handle_dir_up(SD_STATE *state, bool root)
{
   if (root)
      return sd_handle_change_dir(state, (char *)"/", false);

   return sd_handle_change_dir(state, (char *)"..", true);
}

const MENU sd_menu = {
   .state = &sd_state,
   .dir = (uint8_t (*)(void *))sd_handle_dir,
   .dir_up = (uint8_t (*)(void *, bool))sd_handle_dir_up,
   .prev_page = (uint8_t (*)(void *))sd_handle_dir_prev_page,
   .next_page = (uint8_t (*)(void *))sd_handle_dir_next_page,
   .select = (uint8_t (*)(void *, uint8_t, uint8_t))sd_handle_select
};

bool chdir_last(void)
{
   bool res = false;

   // Change to last selected dir if any
   if (cfg_file.path[0]) {
      res = dir_change(cfg_file.path);
      if (!res) {
         cfg_file.path[0] = 0;
         cfg_file.file[0] = 0;
      }

   } else  {
      cfg_file.file[0] = 0;
   }

   return res;
}

const MENU *sd_menu_get(void)
{
   return &sd_menu;
}

const MENU *sd_menu_init(void)
{
   chdir_last();
   sd_state.page_no = 0;
   sd_state.dir_end = true;

   return &sd_menu;
}
