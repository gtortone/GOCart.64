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
 *
 * D64/D71/D81 browser: lists the disk directory and loads the selected PRG
 * file (single file programs only, there is no drive emulation)
 */

#include <stdio.h>
#include <cstring>

#include "menu_d64.h"
#include "menu_image.h"
#include "menu_sd.h"
#include "file_types.h"
#include "cartridge.h"
#include "config.h"

extern const MENU *menu;
extern CFG_FILE cfg_file;

// shared with the other image menu (see menu_image.h)
#define d64_state (image_state.d64)

static uint8_t d64_send_page(D64_STATE *state, uint8_t selected_element) {

   uint8_t element;
   for (element=0; element<MAX_ELEMENTS_PAGE; element++) {

      if (element == 0 && state->page == 0) {
         strcpy(scratch_buf, " ..                               DIR ");
      } else if (element == 1 && state->page == 0) {
         image_format_diskname(scratch_buf, state->image.diskname, state->image.diskname_len);
      } else {
         if (!d64_read_dir(&state->image)) {
            state->dir_end = true;
            send_page_end();
            break;
         }
         D64_ENTRY *e = &state->image.entry;
         image_format_entry(scratch_buf, e->blocks, e->filename, e->type);
      }

      if (element == selected_element)
         scratch_buf[0] = SELECTED_ELEMENT;

      c64_send_data(scratch_buf, ELEMENT_LENGTH);
   }

   return element;
}

static bool d64_skip_to_page(D64_STATE *state, uint16_t page) {

   bool page_found = true;
   state->page = page;

   uint16_t skip = state->page * MAX_ELEMENTS_PAGE;
   for (uint16_t i=2; i<skip; i++) {
      if (!d64_read_dir(&state->image)) {
         page_found = false;
         break;
      }
   }

   if (!page_found) {
      d64_rewind_dir(&state->image);
      state->page = 0;
   }

   return page_found;
}

static uint8_t d64_dir(D64_STATE *state) {

   d64_rewind_dir(&state->image);
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

      if (!d64_skip_to_page(state, page)) {
         cfg_file.img.element = 0;
         selected_element = MAX_ELEMENTS_PAGE;
      }
   }

   d64_send_page(state, selected_element);
   return CMD_READ_DIR;
}

static uint8_t d64_dir_up(D64_STATE *state, bool root) {

   cfg_file.img.element = ELEMENT_NOT_SELECTED;
   d64_close(&state->image);

   menu = sd_menu_get();
   if (root)
      return menu->dir_up(menu->state, root);

   return menu->dir(menu->state);
}

static uint8_t d64_next_page(D64_STATE *state) {

   if (!state->dir_end) {
      state->page++;
      if (!d64_send_page(state, MAX_ELEMENTS_PAGE))
         state->page--;
   } else {
      send_page_end();
   }

   return CMD_READ_DIR_PAGE;
}

static uint8_t d64_prev_page(D64_STATE *state) {

   if (state->page) {
      d64_rewind_dir(&state->image);
      state->dir_end = false;

      d64_skip_to_page(state, state->page-1);
      d64_send_page(state, MAX_ELEMENTS_PAGE);
   } else {
      send_page_end();
   }

   return CMD_READ_DIR_PAGE;
}

static bool d64_is_prg(D64_ENTRY *entry) {
   return (entry->type & 0x07) == CBM_FILE_PRG;
}

static uint8_t d64_select(D64_STATE *state, uint8_t flags, uint8_t element_no) {

   (void)flags;   // file options are not supported for disk entries

   uint16_t element = element_no + state->page * MAX_ELEMENTS_PAGE;
   cfg_file.img.element = element;

   if (element == 0)
      return d64_dir_up(state, false);

   bool first_prg = (element == 1);     // "*": load the first PRG

   d64_rewind_dir(&state->image);
   bool found = false;

   if (first_prg) {
      while ((found = d64_read_dir(&state->image))) {
         if (d64_is_prg(&state->image.entry))
            break;
      }
   } else {
      for (uint16_t i=2; i<=element; i++) {
         if (!(found = d64_read_dir(&state->image)))
            break;
      }
   }

   if (!found) {
      if (first_prg) {
         return handle_unsupported_ex("Not Found",
            "No PRG files were found in image", cfg_file.file);
      }

      cfg_file.img.element = 0;
      return d64_dir(state);
   }

   char name[17];
   image_copy_name(name, state->image.entry.filename);

   if (!d64_is_prg(&state->image.entry))
      return handle_unsupported_ex("Unsupported", "Only PRG files can be loaded", name);

   size_t size = d64_read_prg(&state->image, PRG_BUF, PRG_BUF_SIZE);
   d64_close(&state->image);

   if (!size)
      return handle_unsupported(name);

   // the launcher is restarted in EF3 mode: from here on the menu protocol
   // is no longer available, a failed transfer is only logged
   if (run_prg(PRG_BUF, size, name))
      config_save();      // start from this directory at the next power on
   return CMD_WAIT_SYNC;
}

static const MENU d64_menu = {
   .state = &d64_state,
   .dir = (uint8_t (*)(void *))d64_dir,
   .dir_up = (uint8_t (*)(void *, bool))d64_dir_up,
   .prev_page = (uint8_t (*)(void *))d64_prev_page,
   .next_page = (uint8_t (*)(void *))d64_next_page,
   .select = (uint8_t (*)(void *, uint8_t, uint8_t))d64_select
};

const MENU *d64_menu_init(const char *file_name) {

   d64_state.page = 0;
   d64_state.dir_end = false;

   if (!d64_open(&d64_state.image, file_name))
      return NULL;

   return &d64_menu;
}
