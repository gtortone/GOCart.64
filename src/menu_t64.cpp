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

#include <stdio.h>
#include <cstring>

#include "menu_t64.h"
#include "menu_image.h"
#include "menu_sd.h"
#include "file_types.h"
#include "cartridge.h"
#include "config.h"

extern const MENU *menu;
extern CFG_FILE cfg_file;

// shared with the other image menu (see menu_image.h)
#define t64_state (image_state.t64)

static void t64_format_entry(char *buffer, T64_ENTRY *entry) {

   uint16_t size = (entry->end_address - entry->start_address) + 2;
   uint16_t blocks = (size / 254) + 1;

   image_format_entry(buffer, blocks, entry->filename, entry->file_type);
}

static uint8_t t64_send_page(T64_STATE *state, uint8_t selected_element) {

   uint8_t element;
   for (element=0; element<MAX_ELEMENTS_PAGE; element++) {

      if (element == 0 && state->page == 0) {
         strcpy(scratch_buf, " ..                               DIR ");
      } else if (element == 1 && state->page == 0) {
         image_format_diskname(scratch_buf, state->image.header.user_description, 24);
      } else {
         if (!t64_read_dir(&state->image)) {
            state->dir_end = true;
            send_page_end();
            break;
         }
         t64_format_entry(scratch_buf, &state->image.entry);
      }

      if (element == selected_element)
         scratch_buf[0] = SELECTED_ELEMENT;

      c64_send_data(scratch_buf, ELEMENT_LENGTH);
   }

   return element;
}

static bool t64_skip_to_page(T64_STATE *state, uint16_t page) {

   bool page_found = true;
   state->page = page;

   uint16_t skip = state->page * MAX_ELEMENTS_PAGE;
   for (uint16_t i=2; i<skip; i++) {
      if (!t64_read_dir(&state->image)) {
         page_found = false;
         break;
      }
   }

   if (!page_found) {
      t64_rewind_dir(&state->image);
      state->page = 0;
   }

   return page_found;
}

static uint8_t t64_dir(T64_STATE *state) {

   t64_rewind_dir(&state->image);
   format_path(scratch_buf, true);
   c64_send_data(scratch_buf, DIR_NAME_LENGTH);
   state->dir_end = false;

   // Search for last selected element
   uint8_t selected_element;
   if (cfg_file.img.element == ELEMENT_NOT_SELECTED) {
      state->page = 0;
      cfg_file.img.element = 0;
      selected_element = MAX_ELEMENTS_PAGE;
   } else {
      selected_element = cfg_file.img.element % MAX_ELEMENTS_PAGE;
      uint16_t page = cfg_file.img.element / MAX_ELEMENTS_PAGE;

      if (!t64_skip_to_page(state, page)) {
         cfg_file.img.element = 0;
         selected_element = MAX_ELEMENTS_PAGE;
      }
   }

   t64_send_page(state, selected_element);
   return CMD_READ_DIR;
}

static uint8_t t64_dir_up(T64_STATE *state, bool root) {

   cfg_file.img.element = ELEMENT_NOT_SELECTED;
   t64_close(&state->image);

   menu = sd_menu_get();
   if (root)
      return menu->dir_up(menu->state, root);

   return menu->dir(menu->state);
}

static uint8_t t64_next_page(T64_STATE *state) {

   if (!state->dir_end) {
      state->page++;
      if (!t64_send_page(state, MAX_ELEMENTS_PAGE))
         state->page--;
   } else {
      send_page_end();
   }

   return CMD_READ_DIR_PAGE;
}

static uint8_t t64_prev_page(T64_STATE *state) {

   if (state->page) {
      t64_rewind_dir(&state->image);
      state->dir_end = false;

      t64_skip_to_page(state, state->page-1);
      t64_send_page(state, MAX_ELEMENTS_PAGE);
   } else {
      send_page_end();
   }

   return CMD_READ_DIR_PAGE;
}

static uint8_t t64_select(T64_STATE *state, uint8_t flags, uint8_t element_no) {

   (void)flags;   // file options are not supported for T64 entries

   uint16_t element = element_no + state->page * MAX_ELEMENTS_PAGE;
   cfg_file.img.element = element;

   if (element == 0)
      return t64_dir_up(state, false);

   if (element == 1)
      element = ELEMENT_NOT_SELECTED;     // "*": find first PRG

   t64_rewind_dir(&state->image);
   bool found = false;
   for (uint16_t i=2; i<=element; i++) {
      if (!(found = t64_read_dir(&state->image)))
         break;

      if (element == ELEMENT_NOT_SELECTED) {
         element = 1;
         break;
      }
   }

   if (!found) {
      // File not found
      if (element == 1 || element == ELEMENT_NOT_SELECTED) {
         cfg_file.img.element = 1;
         return handle_unsupported_ex("Not Found",
            "No PRG files were found in image", cfg_file.file);
      }

      cfg_file.img.element = 0;
      return t64_dir(state);
   }

   char name[17];
   image_copy_name(name, state->image.entry.filename);

   size_t size = t64_read_prg(&state->image, PRG_BUF, PRG_BUF_SIZE);
   t64_close(&state->image);

   if (!size)
      return handle_unsupported(name);

   // the launcher is restarted in EF3 mode: from here on the menu protocol
   // is no longer available, a failed transfer is only logged
   if (run_prg(PRG_BUF, size, name))
      config_save();      // start from this directory at the next power on
   return CMD_WAIT_SYNC;
}

static const MENU t64_menu = {
   .state = &t64_state,
   .dir = (uint8_t (*)(void *))t64_dir,
   .dir_up = (uint8_t (*)(void *, bool))t64_dir_up,
   .prev_page = (uint8_t (*)(void *))t64_prev_page,
   .next_page = (uint8_t (*)(void *))t64_next_page,
   .select = (uint8_t (*)(void *, uint8_t, uint8_t))t64_select
};

const MENU *t64_menu_init(const char *file_name) {

   t64_state.page = 0;
   t64_state.dir_end = false;

   if (!t64_open(&t64_state.image, file_name))
      return NULL;

   return &t64_menu;
}
