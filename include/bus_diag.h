#ifndef BUS_DIAG_H
#define BUS_DIAG_H

/*
 * Bus diagnostics (bus_diag.cpp):
 *  - bus timing calibration: edge delay x hold time grid with a test program
 *    (CRT from utils/mkcart_buscal.py)
 *  - edge delay scan with a real EasyFlash cartridge (shell "bus scan",
 *    DEBUG builds), crashes detected by BusCrashDetect
 *  - EasyFlash CPU trace (EF_DEBUG_CPU_TRACE, DEBUG builds, shell "cputrace")
 */

#include <stdint.h>
#include "cartridge.h"
#include "c64_bus.h"
#include "board.h"

/*
 * Bus timing calibration: started by run_cart() for an EasyFlash CRT with
 * this name (not loaded into the PSRAM)
 */
#define BUS_CAL_CRT_NAME   "GOCART BUS CALIBRATION"
void bus_calibrate(core1_args_t *args);

#ifdef DEBUG
/*
 * Edge delay scan (shell "bus scan"): the next EasyFlash cartridge started
 * by run_cart() runs the scan (bus_scan_ef) instead of starting normally
 */
void bus_scan_request(uint32_t seconds, uint32_t from, uint32_t to, uint32_t step, uint32_t repeat);
bool bus_scan_pending(void);
bool bus_scan_running(void);
void bus_scan_ef(core1_args_t *args, bool exrom, bool game);

// crash seen by BusCrashDetect: 0 none, 1 JAM, 2 BRK
extern volatile uint32_t bus_scan_crash;

/*
 * Crash detection, core1, once per cycle with the last CPU phase sample
 * (cpu_trace() method of a cartridge, see c64_bus.h): the 6510 is stuck
 * (JAM: the same read, BA high, repeated BUS_CRASH_JAM_CYCLES times) or ran
 * a BRK (status pushed with B set, then the vector read at $FFFE)
 */
#define BUS_CRASH_JAM_CYCLES   1000

struct BusCrashDetect {

   uint32_t prev;
   uint32_t rep;

   inline __attribute__((always_inline)) void cycle(uint32_t control) {
      uint32_t c = control & ~(1u << 31);
      if (c == prev) {
         if (++rep == BUS_CRASH_JAM_CYCLES && (c & BA_MASK))
            bus_scan_crash = 1;
         return;
      }
      if ((c & BA_MASK) && (c & RW_MASK) && bus_addr(c) == 0xFFFE &&
          !(prev & RW_MASK) && (bus_addr(prev) >> 8) == 0x01 && (bus_data(prev) & 0x10))
         bus_scan_crash = 2;
      prev = c;
      rep = 0;
   }
};
#endif

/*
 * EF_DEBUG_CPU_TRACE (needs a DEBUG build)
 *   1: every C64 cycle of a running EasyFlash cartridge (address, R/W, data,
 *      select lines, BA) is recorded by core1 in a ring buffer at the end of
 *      crt_buf, like a logic analyzer. Recording stops CPU_TRACE_POST cycles
 *      after the CPU reads a trigger address outside the cartridge ROM
 *      (defaults: see cpu_trace_trig_addr), or after the same cycle is
 *      repeated CPU_TRACE_JAM_CYCLES times (JAM, always enabled), or at a BRK
 *      instruction (crash into zeroed memory; "cputrace brk on|off").
 *      Consecutive identical cycles are recorded once (a JAM does not
 *      overwrite the history).
 *      Shell command: cputrace [dump [n] | trig [w]<addr>[=<data>] ... | stop | run]
 *   0: disabled
 */

#ifndef EF_DEBUG_CPU_TRACE
#define EF_DEBUG_CPU_TRACE   0
#endif

// shell "cputrace" command
void cpu_trace_shell(char *args);

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
#define CPU_TRACE_ENTRIES   (32 * 1024)                 // 128K, power of 2
#define CPU_TRACE_BYTES     (CPU_TRACE_ENTRIES * 4)
#define CPU_TRACE_POST      2048                         // cycles kept after the trigger
#define CPU_TRACE_TRIGGERS  2
#define CPU_TRACE_JAM_CYCLES  1000                       // trigger: the same cycle repeated

// ring buffer at the end of crt_buf
extern uint8_t crt_buf[CRT_BUFFER_SIZE];
#define cpu_trace_buf   ((uint32_t *)(crt_buf + CRT_BUFFER_SIZE - CPU_TRACE_BYTES))

extern volatile bool cpu_trace_on;          // buffer available (cartridge fits)
extern volatile bool cpu_trace_frozen;
extern volatile uint32_t cpu_trace_head;     // entries written
extern volatile uint32_t cpu_trace_post;     // entries left after the trigger, 0 = not triggered
extern volatile uint32_t cpu_trace_trig_at;  // entry index of the trigger
// triggers: CPU read of an address, optionally with a given data (0x100 = any)
// defaults: $FE66 KERNAL BRK handler (crash -> "READY"),
//           $EA31 = $00 KERNAL IRQ entry read from RAM, i.e. a BRK (crash
//           with the KERNAL banked out -> black screen)
extern volatile uint32_t cpu_trace_trig_addr[CPU_TRACE_TRIGGERS];
extern volatile uint32_t cpu_trace_trig_data[CPU_TRACE_TRIGGERS];
extern volatile bool cpu_trace_trig_write[CPU_TRACE_TRIGGERS];   // write instead of read
extern uint32_t cpu_trace_last;
extern uint32_t cpu_trace_repeat;            // repetitions of cpu_trace_last
extern volatile bool cpu_trace_brk;          // trigger: BRK instruction

void cpu_trace_reset(bool available);

// core1, once per cycle
static inline __attribute__((always_inline)) void cpu_trace_record(uint32_t control) {

   if (!cpu_trace_on || cpu_trace_frozen)
      return;

   if (control == cpu_trace_last) {
      // the same cycle repeated many times: the 6510 is stuck (JAM: it reads
      // $FFFF at every cycle), trigger at the first one
      if (++cpu_trace_repeat == CPU_TRACE_JAM_CYCLES && !cpu_trace_post && (control & BA_MASK)) {
         cpu_trace_trig_at = cpu_trace_head - 1;
         cpu_trace_post = CPU_TRACE_POST;
      }
      return;
   }

   // BRK: the status register pushed with B set (bit 4), then the vector
   // read at $FFFE (an IRQ pushes it with B clear)
   bool brk = (control & BA_MASK) && (control & RW_MASK) && bus_addr(control) == 0xFFFE &&
              !(cpu_trace_last & RW_MASK) && (bus_addr(cpu_trace_last) >> 8) == 0x01 &&
              (bus_data(cpu_trace_last) & 0x10);

   cpu_trace_repeat = 0;
   cpu_trace_last = control;
   uint32_t head = cpu_trace_head;
   cpu_trace_buf[head & (CPU_TRACE_ENTRIES - 1)] = control;
   cpu_trace_head = head + 1;

   uint32_t post = cpu_trace_post;
   if (post) {
      if (--post == 0)
         cpu_trace_frozen = true;
      cpu_trace_post = post;
   } else if (brk && cpu_trace_brk) {
      cpu_trace_trig_at = head;
      cpu_trace_post = CPU_TRACE_POST;
   } else if (!bus_rom_sel(control) && (control & BA_MASK)) {
      // reads of the cartridge ROM do not trigger (e.g. the EasyFlash boot
      // code copies $FE00-$FEFF from ROMH in Ultimax mode), nor cycles with
      // BA low (VIC-II accesses, e.g. a screen at $FC00)
      uint32_t addr = bus_addr(control);
      bool write = !(control & RW_MASK);
      for (int i = 0; i < CPU_TRACE_TRIGGERS; i++) {
         if (cpu_trace_trig_addr[i] && addr == cpu_trace_trig_addr[i] &&
             write == cpu_trace_trig_write[i] &&
             (cpu_trace_trig_data[i] > 0xFF || bus_data(control) == cpu_trace_trig_data[i])) {
            cpu_trace_trig_at = head;
            cpu_trace_post = CPU_TRACE_POST;
            break;
         }
      }
   }
}
#endif

#endif
