#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "board.h"
#include "i2c_handler.h"
#include "c64_interface.h"
#include "filesystem.h"
#include "cartridge.h"
#include "menu.h"
#include "shell.h"
#include "config.h"

int main(void) {
   
   board_setup();

   c64_set_exrom_game(1, 1);         // <no cartridge>
   c64_hold_reset();
   sleep_ms(250);
   c64_release_reset();

   // mount SD
   if(!filesystem_mount())
      printf("E: SD mount failed\n");
   else
      config_load();    // directory of the last launched file

   // setup I2C irq handler
   i2c_slave_init(i2c0, I2C_ADDR, &i2c_slave_handler);
   i2c_init_regspace();

   sleep_ms(500);

#ifdef DEBUG
   print_prompt(true);
#endif

   run_launcher();
   menu_loop();

#ifdef DEBUG
   while(true) {

      while(!launcher_running()) {
         poll_shell();   // keep shell running after menu_loop()
         cart_poll();    // serve the running cartridge (EAPI)
      }
   }
#else
   // core1 is serving the cartridge: main() must never return
   // (exit() ends in a breakpoint loop -> HardFault without a debugger)
   while(true)
      cart_poll();      // serve the running cartridge (EAPI)
#endif

   return 0;
}

