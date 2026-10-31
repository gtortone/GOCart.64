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
 */

#ifndef SD_MENU_H
#define SD_MENU_H

#include "ff.h"
#include "commands.h"
#include "cartridge.h"
#include "menu.h"

extern uint8_t *crt_banks[BANKS_NUM];
#define scratch_buf  ((char *)CRT_BANK(1))    // 16 kb

typedef struct {
   bool in_root;
   bool dir_end;

   DIR start_page;
   DIR end_page;
   uint16_t page_no;

   bool ignore_crt_updated;

   char search[SEARCH_LENGTH+2];
} SD_STATE;

void sd_format_size(char *buffer, uint32_t size);
void sd_format_element(char *buffer, FILINFO *info);
void sd_send_not_found(SD_STATE *state);
uint8_t sd_send_page(SD_STATE *state, uint8_t selected_element);
void sd_dir_open(SD_STATE *state);
void sd_send_prg_message(const char *message);
void sd_send_warning_restart(const char *message, const char *filename);
uint8_t sd_parse_file_number(char *filename, uint8_t *extension);
bool sd_generate_new_filename(void);
void sd_handle_save_updated_crt(uint8_t flags);
bool sd_crt_updated(SD_STATE *state);

uint8_t sd_handle_dir(SD_STATE *state);
uint8_t sd_handle_change_dir(SD_STATE *state, char *path, bool select_old);
uint8_t sd_handle_dir_next_page(SD_STATE *state);
uint8_t sd_handle_dir_prev_page(SD_STATE *state);
uint8_t sd_handle_delete_file(const char *file_name);
void sd_file_open(FIL *file, const char *file_name);
uint8_t sd_handle_crt_unsupported(uint32_t cartridge_type);
bool sd_c128_only_warning(uint8_t flags);
uint8_t sd_handle_c128_only_warning(const char *file_name, uint8_t element);
uint8_t sd_handle_load(SD_STATE *state, const char *file_name, uint8_t file_type, uint8_t flags, uint8_t element);
uint8_t sd_handle_select(SD_STATE *state, uint8_t flags, uint8_t element);
uint8_t sd_handle_dir_up(SD_STATE *state, bool root);
bool chdir_last(void);

const MENU *sd_menu_init(void);
const MENU *sd_menu_get(void);

#endif
