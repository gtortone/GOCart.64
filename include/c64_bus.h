#ifndef C64_BUS_H
#define C64_BUS_H

#include <type_traits>
#include "pico/stdlib.h"
#include "board.h"
#include "c64_interface.h"

/*
 * Generic C64 expansion port bus loop, synchronized to PHI2.
 *
 * Every C64 cycle is split in two phases:
 *
 *  - CPU phase (PHI2 high): the bus is sampled continuously until PHI2 falls.
 *    Read data for the latest address is kept driven for the whole phase
 *    (the CPU latches it on the PHI2 falling edge); ROML/ROMH/IO1/IO2 are
 *    asserted by the PLA some tens of ns after PHI2 rises, so a single sample
 *    on the rising edge is not enough. This also covers VIC-II fetches done
 *    while the CPU is stopped (bad lines, sprites), when R/W is pulled high.
 *
 *  - commit (PHI2 falling edge): side effects (register writes, bank switching
 *    on read, pointer increments) are applied once per cycle, using the last
 *    CPU-phase sample. 6510 write data becomes valid well after PHI2 rises, so
 *    the last sample is the only reliable one. This is also when the real
 *    cartridges latch their registers.
 *
 *  - VIC-II phase (PHI2 low): only in Ultimax mode the VIC-II sees ROMH at
 *    $3000-$3FFF of every VIC bank (A12-A15 pulled up -> cartridge sees $Fxxx).
 *    The bus is sampled continuously and the data for the latest address is
 *    kept driven until PHI2 rises, when the VIC-II has latched it.
 *
 * Cart interface (all methods must be inline and free of flash accesses):
 *
 *   static constexpr uint32_t COMMIT_SEL
 *       select line masks (active low, any of IO1/IO2/ROML/ROMH) whose cycles
 *       are passed to commit(); 0 if the cartridge has no registers
 *
 *   bool cpu_read(uint32_t control, uint8_t &data)
 *       CPU phase, R/W high: return true and set data to drive the bus.
 *       Called many times per cycle: it must not have side effects
 *
 *   void commit(uint32_t control)
 *       called once per cycle on the PHI2 falling edge, with the last CPU-phase
 *       sample in which one of the COMMIT_SEL lines was asserted
 *       (check RW_MASK to tell reads from writes)
 *
 *   bool vic_enabled(void)
 *       true when the VIC-II phase must be served (Ultimax mode)
 *
 *   uint8_t vic_read(uint32_t addr)
 *       data for a VIC-II fetch through ROMH
 *
 *   void early_commit(uint32_t control)   (optional)
 *       called at every sample of a write cycle on a COMMIT_SEL line, during
 *       the CPU phase (from the C64_BUS_EARLY_COMMIT_SAMPLES-th sample). Only
 *       for idempotent registers that must take effect at the very next cycle,
 *       e.g. disabling the cartridge from ROM: EXROM/GAME are released and
 *       raised by the C64 pull-ups, which takes time. The first samples may
 *       carry a value that is not valid yet: it is overwritten by the next
 *       ones. commit() is still called at the end of the cycle.

 */

#define BUS_INLINE   static inline __attribute__((always_inline))

/*
 * Bus timing tuning, in CPU cycles (4 ns each @ 250 MHz). 0 = disabled.
 *
 * C64_BUS_EDGE_DELAY_CYCLES
 *   delay after every PHI2 edge before the bus is sampled. Masks select lines
 *   (ROML/ROMH/IO1/IO2) that are stale from the previous phase or glitch while
 *   the PLA inputs (AEC, address) change. Matters mostly in Ultimax mode, where
 *   ROMH is used in both phases. When C64_BUS_HOLD_CYCLES is set, the delay
 *   starts after the hold time of the previous phase.
 *
 * C64_BUS_HOLD_CYCLES
 *   time the data bus is still driven after a PHI2 edge ends a phase. The 6510
 *   latches read data slightly after PHI2 falls on the expansion port; a real
 *   ROM keeps its outputs valid for a few tens of ns after /OE is released,
 *   while the RP2350 releases the bus immediately. Matters mostly in Ultimax
 *   mode, where every opcode is fetched from ROMH.
 */
#ifndef C64_BUS_EDGE_DELAY_CYCLES
#define C64_BUS_EDGE_DELAY_CYCLES   0
#endif

#ifndef C64_BUS_HOLD_CYCLES
#define C64_BUS_HOLD_CYCLES         10
#endif

/*
 * early_commit(): first sample of a write cycle (on a COMMIT_SEL line) passed
 * to early_commit(). 1 = from the first sample, like the original firmware
 * (6510 write data is valid ~150 ns after PHI2 rises, a sample takes ~50 ns).
 */
#ifndef C64_BUS_EARLY_COMMIT_SAMPLES
#define C64_BUS_EARLY_COMMIT_SAMPLES   1
#endif

BUS_INLINE void bus_edge_delay(void) {
   if (C64_BUS_EDGE_DELAY_CYCLES > 0)
      busy_wait_at_least_cycles(C64_BUS_EDGE_DELAY_CYCLES);
}

// release the data bus at the end of a phase, after the hold time
BUS_INLINE void bus_release(void) {
   if (C64_BUS_HOLD_CYCLES > 0)
      busy_wait_at_least_cycles(C64_BUS_HOLD_CYCLES);
   SET_DATA_MODE_IN
}

BUS_INLINE uint32_t bus_addr(uint32_t control) {
   return (control & ADDR_GPIO_MASK) >> PINROMADDR;
}

BUS_INLINE uint8_t bus_data(uint32_t control) {
   return (control & DATA_GPIO_MASK) >> PINROMDATA;
}

// true if ROML or ROMH is asserted
BUS_INLINE bool bus_rom_sel(uint32_t control) {
   return (control & (ROML_MASK|ROMH_MASK)) != (ROML_MASK|ROMH_MASK);
}

// detect the optional early_commit() method
template <typename T, typename = void>
struct bus_has_early_commit : std::false_type {};
template <typename T>
struct bus_has_early_commit<T, std::void_t<decltype(&T::early_commit)>> : std::true_type {};

template <typename Cart>
BUS_INLINE void c64_bus_loop(Cart &cart) {

   uint32_t control;
   uint32_t sel_control = 0;
   bool sel_pending;

   // early_commit() state
   uint32_t early_samples;

   SET_DATA_MODE_IN

   // start from a PHI2 rising edge
   wait_low(PHI2);
   wait_high(PHI2);

   while(1) {

      //
      // CPU phase (PHI2 high)
      //
      sel_pending = false;
      early_samples = 0;

      bus_edge_delay();

      while(1) {

         GPIO_GET_LOW_32(control);
         if (!(control & PHI2_MASK))
            break;

         if ((control & Cart::COMMIT_SEL) != Cart::COMMIT_SEL) {
            sel_control = control;
            sel_pending = true;

            if constexpr (bus_has_early_commit<Cart>::value) {
               // write cycle: apply at every sample, the last one wins
               if (!(control & RW_MASK) && ++early_samples >= C64_BUS_EARLY_COMMIT_SAMPLES)
                  cart.early_commit(control);
            }
         }

         uint8_t data;
         if ((control & RW_MASK) && cart.cpu_read(control, data)) {
            DATA_OUT(data);
            SET_DATA_MODE_OUT
         } else {
            SET_DATA_MODE_IN
         }
      }

      bus_release();

      //
      // commit (PHI2 falling edge)
      //
      if (sel_pending)
         cart.commit(sel_control);

      //
      // VIC-II phase (PHI2 low)
      //
      if (cart.vic_enabled()) {

         bus_edge_delay();

         while(1) {

            GPIO_GET_LOW_32(control);
            if (control & PHI2_MASK)
               break;

            if (!(control & ROMH_MASK)) {
               DATA_OUT(cart.vic_read(bus_addr(control)));
               SET_DATA_MODE_OUT
            } else {
               SET_DATA_MODE_IN
            }
         }

         bus_release();

      } else {

         wait_high(PHI2);
      }
   }
}

#endif
