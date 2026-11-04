
#include "pico/stdlib.h"

#include "c64_interface.h"
#include "board.h"
#include "dma_bus.pio.h"
#include "c64_bus.h"

// bus loop timing (see c64_bus.h), set from the configuration file
volatile uint32_t c64_bus_edge_cycles = C64_BUS_EDGE_DELAY_CYCLES;
volatile uint32_t c64_bus_hold_cycles = C64_BUS_HOLD_CYCLES;

uint dma_bus_offset;

void c64_dma_bus_init(void) {
   dma_bus_offset = pio_add_program(DMA_BUS_PIO, &dma_bus_program);
   pio_sm_config c = dma_bus_program_get_default_config(dma_bus_offset);
   sm_config_set_in_pins(&c, PHI2);
   sm_config_set_jmp_pin(&c, BA);
   sm_config_set_out_pins(&c, GC_ADDR0_PIN, DMA_BUS_PINS);
   sm_config_set_set_pins(&c, GC_ADDR0_PIN, 1);
   sm_config_set_clkdiv(&c, 1.0f);
   pio_sm_init(DMA_BUS_PIO, DMA_BUS_SM, dma_bus_offset, &c);
   // pin values: A0-A15 = $FFFF, R/W = read; not driven (pindirs 0) and the
   // pins stay SIO inputs until c64_dma_bus_start()
   pio_sm_set_pins_with_mask(DMA_BUS_PIO, DMA_BUS_SM, 0x1FFFFu << GC_ADDR0_PIN,
                             0x1FFFFu << GC_ADDR0_PIN);
   pio_sm_set_pindirs_with_mask(DMA_BUS_PIO, DMA_BUS_SM, 0, 0x1FFFFu << GC_ADDR0_PIN);

}

void c64_hold_reset(void) {
   gpio_put(RESET,1);
}

void c64_release_reset(void) {
   gpio_put(RESET,0);
}

void c64_reset(void) {
   c64_hold_reset();
   // busy wait: sleep_us() needs the timer interrupt of the alarm pool,
   // served by core0, and core1 calls this function at every cartridge start
   busy_wait_us_32(200);
   c64_release_reset();
}
