#ifndef C64_INTERFACE_
#define C64_INTERFACE_

#include "board.h"
#include "hardware/pio.h"
#include "hardware/structs/io_bank0.h"

void c64_hold_reset(void);
void c64_release_reset(void);
void c64_reset(void);

inline void c64_set_exrom_game(bool exrom, bool game) {
   gpio_put(EXROM, !exrom);
   gpio_put(GAME, !game);
}

// DMA line (open drain, see board_setup): the 6510 stops at its next read
// cycle (only assert it when the next CPU cycle is a read, a CPU write cycle
// would be corrupted)
inline void c64_dma_assert(void) {
   gpio_set_dir(DMA, GPIO_OUT);     // output value is 0
}

inline void c64_dma_release(void) {
   gpio_set_dir(DMA, GPIO_IN);
}

/*
 * Bus driver during DMA (dma_bus.pio): A0-A15 and R/W are driven in the PHI2
 * high phases while the 6510 is stopped (a floating bus corrupts the DRAM).
 * c64_dma_bus_start() after c64_dma_assert(), c64_dma_bus_stop() in a PHI2
 * low phase, before c64_dma_release(). Code in RAM (inline, no SDK calls).
 */
#define DMA_BUS_PIO       pio0
#define DMA_BUS_SM        0
#define DMA_BUS_PINS      17          // A0-A15, R/W
static_assert(GC_RW_PIN == GC_ADDR0_PIN + 16, "dma_bus.pio: R/W must follow A15");

extern uint dma_bus_offset;

// load the program (core0, at boot)
void c64_dma_bus_init(void);

// The function select of the 17 pins is switched to the PIO at the first
// pause only and left there (the SIO still reads them; with the SM stopped
// and pindirs 0 they are not driven): 17 APB writes take ~0.4 us, too long
// for the PHI2 low phase in which the DMA line is released.
// c64_dma_bus_restore() gives them back to the SIO (cartridge stop).
inline void c64_dma_bus_start(void) {
   if ((io_bank0_hw->io[GC_ADDR0_PIN].ctrl & IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS) != GPIO_FUNC_PIO0) {
      for (uint i = 0; i < DMA_BUS_PINS; i++)
         io_bank0_hw->io[GC_ADDR0_PIN + i].ctrl = GPIO_FUNC_PIO0;
   }
   DMA_BUS_PIO->sm[DMA_BUS_SM].instr = pio_encode_jmp(dma_bus_offset);
   hw_set_bits(&DMA_BUS_PIO->ctrl, 1u << (PIO_CTRL_SM_ENABLE_LSB + DMA_BUS_SM));
}

inline void c64_dma_bus_stop(void) {
   hw_clear_bits(&DMA_BUS_PIO->ctrl, 1u << (PIO_CTRL_SM_ENABLE_LSB + DMA_BUS_SM));
   DMA_BUS_PIO->sm[DMA_BUS_SM].instr = pio_encode_mov(pio_pindirs, pio_null);
}

// pins back to the SIO (not in a DMA pause: slow)
inline void c64_dma_bus_restore(void) {
   c64_dma_bus_stop();
   for (uint i = 0; i < DMA_BUS_PINS; i++)
      io_bank0_hw->io[GC_ADDR0_PIN + i].ctrl = GPIO_FUNC_SIO;
}

inline void wait_low(int line) {
   while(sio_hw->gpio_in & (1u << line))
      tight_loop_contents();
}

inline void wait_high(int line) {
   while(!(sio_hw->gpio_in & (1u << line)))
      tight_loop_contents();
}

inline void wait_high(int line, int ntry) {
   int i=0;
   while(!(sio_hw->gpio_in & (1u << line)) && i++<ntry)
      tight_loop_contents();
}

#endif
