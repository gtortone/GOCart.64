#include <stdio.h>      // printf
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"

#include "cartridge-pio.h"
#include "board.h"
#include "c64_interface.h"
#include "rom_normal.pio.h"
#include "rom_ocean.pio.h"
#include "rom_magic.pio.h"
#include "rom_funplay.pio.h"
#include "rom_sgames.pio.h"
#include "rom_dinamic.pio.h"
#include "rom_zaxxon.pio.h"
#include "rom_easyflash.pio.h"

typedef struct {
   uint8_t *crt_buf;
   uint8_t **crt_banks;
   uint8_t *crt_map;
} core1_args_t;

void __time_critical_func(run_cart_normal_pio)(void) {

   volatile uint32_t addr;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t *rom = p->crt_buf;

   PIO pio = pio0;
   uint sm0 = 0;

   // SM0

   uint offset0 = pio_add_program(pio, &rom_normal_program);
   pio_sm_config c0 = rom_normal_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 31, false);

   sm_config_set_in_shift(&c0, false, false, 8);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, GPIO_IN);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0
   
   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);
   
   c64_reset();

   while(1) {
      addr = pio_sm_get_blocking(pio, sm0);
      pio_sm_put_blocking(pio, sm0, rom[addr & 0x3FFF]);
   }
}

//

void __time_critical_func(run_cart_magic_desk_pio)(void) {

   volatile uint32_t addr;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *rom_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;

   // SM0
   uint offset0 = pio_add_program(pio, &rom_magic_program);
   pio_sm_config c0 = rom_magic_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    // BA PHI2 IO2 IO1 ROMH ROML RW A15 ... A0
   for(int i=0; i<=22; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 22, false);

   sm_config_set_in_shift(&c0, false, false, 1);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, false);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0

   sm_config_set_jmp_pin(&c0, 19);     // IO1
   
   // SM1
   uint offset1 = pio_add_program(pio, &rom_magic_bs_program);
   pio_sm_config c1 = rom_magic_bs_program_get_default_config(offset1);

   sm_config_set_in_pins(&c1, 0);    // data 
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 31, false);

   sm_config_set_in_shift(&c1, false, false, 1);

   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);

   pio_sm_init(pio, sm1, offset1, &c1);
   pio_sm_set_enabled(pio, sm1, true);

   c64_reset();

   while(1) {

      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
         addr = pio_sm_get(pio, sm0);
         //printf("A: 0x%lX\n", addr);
         pio_sm_put(pio, sm0, rom_ptr[addr & 0x1FFF]);
      }

      if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {
         uint32_t value = pio_sm_get(pio, sm1);
         uint16_t addr = value & 0xFFFF;
         uint8_t data = value >> 23;
         if( !(addr & 0xFF) ) {
            if ( !(data & 0x80) ) {
               c64_set_exrom_game(0, 1);
               rom_ptr = banks[(data >> 1) & 0x3f];
               //printf("data: %d\n", data);
               if (data & 0x01) {
                  // Use ROMH location for odd banks
                  rom_ptr += 0x2000;
               }
            } else {
               c64_set_exrom_game(1, 1);
            }
         }
      }
   }
}

//

void __time_critical_func(run_cart_ocean_pio)(void) {

   volatile uint32_t addr;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *rom_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;

   // SM0
   uint offset0 = pio_add_program(pio, &rom_ocean_program);
   pio_sm_config c0 = rom_ocean_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    // BA PHI2 IO2 IO1 ROMH ROML RW A15 ... A0
   for(int i=0; i<=22; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 22, false);

   sm_config_set_in_shift(&c0, false, false, 1);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, false);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0
   
   // SM1
   uint offset1 = pio_add_program(pio, &rom_ocean_bs_program);
   pio_sm_config c1 = rom_ocean_bs_program_get_default_config(offset1);

   sm_config_set_in_pins(&c1, 0);    // data 
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 31, false);

   sm_config_set_set_pins(&c1, 23, 8); // D7 ... D0

   sm_config_set_jmp_pin(&c1, 19); // IO1

   sm_config_set_in_shift(&c1, false, false, 1);

   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);

   pio_sm_init(pio, sm1, offset1, &c1);
   pio_sm_set_enabled(pio, sm1, true);

   c64_reset();

   while(1) {

      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
         addr = pio_sm_get(pio, sm0);
         pio_sm_put(pio, sm0, rom_ptr[addr & 0x3FFF]);
      }

      if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {
         uint32_t value = pio_sm_get(pio, sm1);
         if ( (value & 0xFFFF) == 0xDE00) {
            uint8_t bank = value >> 23;
            rom_ptr = banks[bank % 16];
         }
      }
   }
}

//

void __time_critical_func(run_cart_fun_play_pio)(void) {

   volatile uint32_t addr;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *rom_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;

   // SM0
   uint offset0 = pio_add_program(pio, &rom_funplay_program);
   pio_sm_config c0 = rom_funplay_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    // BA PHI2 IO2 IO1 ROMH ROML RW A15 ... A0
   for(int i=0; i<=22; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 22, false);

   sm_config_set_in_shift(&c0, false, false, 1);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, false);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0
   
   // SM1
   uint offset1 = pio_add_program(pio, &rom_funplay_bs_program);
   pio_sm_config c1 = rom_funplay_bs_program_get_default_config(offset1);

   sm_config_set_in_pins(&c1, 0);    // data 
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 31, false);

   sm_config_set_in_shift(&c1, false, false, 1);

   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);

   pio_sm_init(pio, sm1, offset1, &c1);
   pio_sm_set_enabled(pio, sm1, true);

   c64_reset();

   while(1) {

      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
         addr = pio_sm_get(pio, sm0);
         //printf("A: 0x%lX\n", addr);
         pio_sm_put(pio, sm0, rom_ptr[addr & 0x3FFF]);
      }

      if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {
         uint32_t value = pio_sm_get(pio, sm1);
         uint16_t addr = value & 0xFFFF;
         uint8_t data = value >> 23;
         if( (addr == 0xDE00) ) {
            if ( !(data & 0x80)) {
               c64_set_exrom_game(0, 1);
               rom_ptr = banks[( (data >> 3) & 0x07 ) | ( (data & 0x01) << 3)];
            } else {
                c64_set_exrom_game(1, 1);
            }
         }
      }
   }
}

//

void __time_critical_func(run_cart_super_games_pio)(void) {

   volatile uint32_t addr;
   bool disable = false;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *rom_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;

   // SM0
   uint offset0 = pio_add_program(pio, &rom_sgames_program);
   pio_sm_config c0 = rom_sgames_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    // BA PHI2 IO2 IO1 ROMH ROML RW A15 ... A0
   for(int i=0; i<=22; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 22, false);

   sm_config_set_in_shift(&c0, false, false, 1);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, false);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0
   
   sm_config_set_jmp_pin(&c0, 16); // RW

   // SM1
   uint offset1 = pio_add_program(pio, &rom_sgames_bs_program);
   pio_sm_config c1 = rom_sgames_bs_program_get_default_config(offset1);

   sm_config_set_in_pins(&c1, 0);    // data 
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 31, false);

   sm_config_set_in_shift(&c1, false, false, 1);

   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);

   pio_sm_init(pio, sm1, offset1, &c1);
   pio_sm_set_enabled(pio, sm1, true);

   c64_reset();

   while(1) {

      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
         addr = pio_sm_get(pio, sm0);
         //printf("A: 0x%lX\n", addr);
         pio_sm_put(pio, sm0, rom_ptr[addr & 0x3FFF]);
      }

      if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {
         if (!disable) {
            uint32_t value = pio_sm_get(pio, sm1);
            uint8_t data = value >> 23;
            rom_ptr = banks[data & 0x03]; 
            if(data & 0x04) {
               c64_set_exrom_game(1, 1);
            } else {
               c64_set_exrom_game(0, 0);
            }

            if(data & 0x00)
               disable = true;
         }
      }
   }

}

//

__attribute__((optimize("O3"), hot))
void __time_critical_func(run_cart_easyflash_pio)(void) {

   volatile uint32_t value;
   volatile uint16_t addr;
   volatile uint8_t data;
   volatile uint8_t ram_buf[256] = {};
   volatile uint8_t mode = 0;
   uint8_t ef_control[8][2] = {
      {1, 0},  // Ultimax
      {1, 1},  // none
      {0, 0},  // 16K
      {0, 1},  // 8K
      {1, 0},  // Ultimax
      {1, 1},  // none
      {0, 0},  // 16K
      {0, 1}   // 8K
   };

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *crt_map = p->crt_map;
   uint8_t *rom_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;
   uint sm2 = 2;

   // init PIO0 pins
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);

   // SM0
   uint offset0 = pio_add_program(pio, &rom_easyflash_program);
   pio_sm_config c0 = rom_easyflash_program_get_default_config(offset0);
   sm_config_set_in_pins(&c0, 0);    // IO2 IO1 ROMH ROML RW A15 ... A0
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 31, false);
   sm_config_set_in_shift(&c0, false, false, 0);
   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0
   sm_config_set_jmp_pin(&c0, 16); // RW
   sm_config_set_clkdiv(&c0, 1.0f);

   // SM1
   uint offset1 = pio_add_program(pio, &rom_easyflash_bs_program);
   pio_sm_config c1 = rom_easyflash_bs_program_get_default_config(offset1);
   sm_config_set_in_pins(&c1, 0);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 31, false);
   sm_config_set_in_shift(&c1, false, false, 1);
   sm_config_set_jmp_pin(&c1, 16); // RW
   sm_config_set_fifo_join(&c1, PIO_FIFO_JOIN_RX);
   sm_config_set_clkdiv(&c1, 1.0f);
   
   // SM2
   uint offset2 = pio_add_program(pio, &rom_easyflash_write_ram_program);
   pio_sm_config c2 = rom_easyflash_write_ram_program_get_default_config(offset2);
   sm_config_set_in_pins(&c2, 0);
   pio_sm_set_consecutive_pindirs(pio, sm2, 0, 31, false);
   sm_config_set_in_shift(&c2, false, false, 1);
   sm_config_set_jmp_pin(&c2, 16); // RW
   sm_config_set_fifo_join(&c2, PIO_FIFO_JOIN_RX);
   sm_config_set_clkdiv(&c2, 1.0f);

   // load SM
   if (pio_sm_init(pio, sm0, offset0, &c0) < 0)
      printf("SM0: init error");
   pio_sm_set_enabled(pio, sm0, true);

   if (pio_sm_init(pio, sm1, offset1, &c1) < 0)
      printf("SM1: init error");
   pio_sm_set_enabled(pio, sm1, true);

   if (pio_sm_init(pio, sm2, offset2, &c2) < 0)
      printf("SM2: init error");
   pio_sm_set_enabled(pio, sm2, true);

   c64_reset();

   while(1) {
      
      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {

         value = pio_sm_get(pio, sm0);
         addr = value & 0xFFFF;
         //printf("[SM0] V: 0x%lX\n", value);

         if (! (value & IO2_MASK) ) {
            pio_sm_put_blocking(pio, sm0, ram_buf[addr & 0xFF]);
         } else {
            pio_sm_put_blocking(pio, sm0, rom_ptr[addr & 0x3FFF]);
         }

      } else {

         if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {

            value = pio_sm_get(pio, sm1);
            data = value >> 23;

            switch (value & 0xff) {

               case 0x00:
                  rom_ptr = banks[crt_map[data & 0x3F]];
                  break;

               case 0x02:
                  mode = (((data >> 5) & 0x04) | (data & 0x02) | (((data >> 2) & 0x01) & ~data)) & 0x07;
                  c64_set_exrom_game(ef_control[mode][0], ef_control[mode][1]);
                  break;
            }     
            wait_high(IO1);
         }

         if (!pio_sm_is_rx_fifo_empty(pio, sm2)) {

            value = pio_sm_get(pio, sm2);
            data = value >> 23;

            ram_buf[value & 0xFF] = data;

            wait_high(IO2);
         }
      }
   } // end while
}

//

void __time_critical_func(run_cart_dinamic_pio)(void) {

   volatile uint32_t addr;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *rom_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;

   // SM0
   uint offset0 = pio_add_program(pio, &rom_dinamic_program);
   pio_sm_config c0 = rom_dinamic_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    // BA PHI2 IO2 IO1 ROMH ROML RW A15 ... A0
   for(int i=0; i<=22; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 22, false);

   sm_config_set_in_shift(&c0, false, false, 1);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, false);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0

   sm_config_set_jmp_pin(&c0, 19);     // IO1
   
   // SM1
   uint offset1 = pio_add_program(pio, &rom_dinamic_bs_program);
   pio_sm_config c1 = rom_dinamic_bs_program_get_default_config(offset1);

   sm_config_set_in_pins(&c1, 0);    // data 
   for(int i=0; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 31, false);

   sm_config_set_in_shift(&c1, false, false, 1);

   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);

   pio_sm_init(pio, sm1, offset1, &c1);
   pio_sm_set_enabled(pio, sm1, true);

   c64_reset();

   while(1) {

      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
         addr = pio_sm_get(pio, sm0);
         //printf("A: 0x%lX\n", addr);
         pio_sm_put(pio, sm0, rom_ptr[addr & 0x1FFF]);
      }

      if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {
         uint32_t value = pio_sm_get(pio, sm1);
         //printf("B: 0x%lX\n", addr & 0xF);
         rom_ptr = banks[value & 0xF];
      }
   }

}

//

void __time_critical_func(run_cart_zaxxon_pio)(void) {

   volatile uint32_t addr;

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t **banks = p->crt_banks;
   uint8_t *rom_ptr = banks[0];
   uint8_t *rom0_ptr = banks[0];

   PIO pio = pio0;
   uint sm0 = 0;
   uint sm1 = 1;

   // SM0
   uint offset0 = pio_add_program(pio, &rom_zaxxon_rl_program);
   pio_sm_config c0 = rom_zaxxon_rl_program_get_default_config(offset0);

   sm_config_set_in_pins(&c0, 0);    // BA PHI2 IO2 IO1 ROMH ROML RW A15 ... A0
   for(int i=0; i<=22; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 0, 22, false);

   sm_config_set_in_shift(&c0, false, false, 1);

   sm_config_set_out_pins(&c0, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm0, 23, 8, false);

   sm_config_set_set_pins(&c0, 23, 8); // D7 ... D0

   sm_config_set_jmp_pin(&c0, 17);

   // SM1
   uint offset1 = pio_add_program(pio, &rom_zaxxon_rh_program);
   pio_sm_config c1 = rom_zaxxon_rh_program_get_default_config(offset1);

   sm_config_set_in_pins(&c1, 0);    // A15 ... A0
   for(int i=0; i<=15; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 0, 16, false);

   sm_config_set_in_shift(&c1, false, false, 1);

   sm_config_set_out_pins(&c1, 23, 8);  // D7 ... D0
   for(int i=23; i<=30; i++)
      pio_gpio_init(pio, i);
   pio_sm_set_consecutive_pindirs(pio, sm1, 23, 8, false);

   sm_config_set_set_pins(&c1, 23, 8); // D7 ... D0

   sm_config_set_jmp_pin(&c1, 18);

   // load SM
   pio_sm_init(pio, sm0, offset0, &c0);
   pio_sm_set_enabled(pio, sm0, true);

   pio_sm_init(pio, sm1, offset1, &c1);
   pio_sm_set_enabled(pio, sm1, true);

   c64_reset();

   while(1) {

      if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
         // ROML
         addr = pio_sm_get(pio, sm0);
         //printf("A: 0x%lX\n", addr);
         pio_sm_put(pio, sm0, rom0_ptr[addr & 0x0FFF]);
         rom_ptr = banks[addr & 0x1000 ? 1 : 0];
      } else if (!pio_sm_is_rx_fifo_empty(pio, sm1)) {
         // ROMH
         addr = pio_sm_get(pio, sm1);
         //printf("A: 0x%lX\n", addr);
         pio_sm_put(pio, sm1, rom_ptr[addr & 0x3FFF]);
      }
   }
}

//

