
#include "pico/stdlib.h"

#include "c64_interface.h"
#include "board.h"

void c64_hold_reset(void) {
   gpio_put(RESET,1);
}

void c64_release_reset(void) {
   gpio_put(RESET,0);
}

void c64_reset(void) {
   c64_hold_reset();
   sleep_us(200);
   c64_release_reset();
}
