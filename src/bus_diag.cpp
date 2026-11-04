/*
 * Bus diagnostics (see bus_diag.h): bus timing calibration, edge delay scan
 * with a real cartridge, EasyFlash CPU trace
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>     // strtoul
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include "cartridge.h"
#include "cartridge_int.h"
#include "c64_interface.h"
#include "c64_bus.h"
#include "board.h"
#include "config.h"
#include "bus_diag.h"

/*
 * Bus timing calibration
 *
 * Started by a CRT named BUS_CAL_CRT_NAME (utils/mkcart_buscal.py, an
 * EasyFlash test program). For every pair of the grid edge delay x hold time
 * (c64_bus_edge_cycles / c64_bus_hold_cycles, see c64_bus.h) the C64 is reset
 * and runs the test program with that timing: it receives a header through
 * the EF3 USB registers (values under test, results so far, shown on the C64
 * screen), runs the tests (ROM reads in 16K and Ultimax mode, also with
 * other VIC-II banks and the display off, IO2 RAM and code, VIC-II fetches
 * from ROMH, write bursts to I/O registers that must not
 * become cartridge register writes: select line glitches at the start of the
 * CPU phase, masked only by a long enough edge delay) and sends back the
 * number of errors. No answer within CAL_RESULT_US means the C64 crashed:
 * that pair fails too.
 * The chosen pair is the mean of the passing pairs (the middle of the
 * working area: margin for temperature and the like), checked again
 * CAL_CONFIRM times (else the nearest passing pair that is), then set and
 * saved in the configuration file. The last start shows the result.
 * Core0 stays here for the whole scan (a few minutes); progress on the
 * C64 screen and on the debug UART.
 */
#define CAL_EDGE_MAX     30
#define CAL_EDGE_STEP    5
#define CAL_HOLD_MAX     20
#define CAL_HOLD_STEP    2
#define CAL_NE           (CAL_EDGE_MAX / CAL_EDGE_STEP + 1)
#define CAL_NH           (CAL_HOLD_MAX / CAL_HOLD_STEP + 1)
#define CAL_N            (CAL_NE * CAL_NH)     // <= 255 (one byte on the C64)
#define CAL_HEADER_US    (2 * 1000 * 1000)     // C64 start -> header read
#define CAL_RESULT_US    (8 * 1000 * 1000)     // header -> result (~3 s)
#define CAL_CONFIRM      3
#define CAL_SKIP_AFTER   2                     // consecutive failures after a pass
#define CAL_MAGIC        0xC5

static_assert(CAL_N <= 255, "calibration grid too large");

enum { CAL_UNTESTED = 0, CAL_PASS = 1, CAL_FAIL = 2 };
// results, index = hold step * CAL_NE + edge step: in crt_buf (the test
// program uses only its first chunks, the CPU trace the last 128K), RAM is full
static uint8_t * const cal_map = crt_buf + CRT_BUFFER_SIZE / 2;

static bool cal_putc(uint8_t data, uint32_t timeout_us) {
   uint64_t t0 = time_us_64();
   while (ef3_rx_rdy) {
      if (time_us_64() - t0 > timeout_us)
         return false;
   }
   ef3_rx_data = data;
   __dmb();
   ef3_rx_rdy = EF3_RX_RDY;
   return true;
}

static bool cal_getc(uint8_t *data, uint32_t timeout_us) {
   uint64_t t0 = time_us_64();
   while (ef3_tx_rdy) {
      if (time_us_64() - t0 > timeout_us)
         return false;
   }
   *data = ef3_tx_data;
   __dmb();
   ef3_tx_rdy = EF3_TX_RDY;
   return true;
}

// restart the C64 with the test program and the given bus timing
static void cal_start(core1_args_t *args, uint32_t edge, uint32_t hold) {
   multicore_reset_core1();
   c64_dma_bus_restore();
   c64_dma_release();
   c64_hold_reset();
   ef3_rx_rdy = EF3_NOT_RDY;
   ef3_tx_rdy = EF3_TX_RDY;
   c64_bus_edge_cycles = edge;
   c64_bus_hold_cycles = hold;
   c64_set_exrom_game(1, 0);       // EasyFlash boots in Ultimax mode
   multicore_launch_core1(run_cart_easyflash);
   multicore_fifo_push_blocking((uint32_t)args);
}

// header for the test program: mode 0 test (pair idx), 1 result, 2 no pair
static bool cal_header(uint8_t mode, uint32_t edge, uint32_t hold, uint32_t idx, bool saved) {
   const uint8_t h[] = { CAL_MAGIC, mode, (uint8_t)edge, (uint8_t)hold, (uint8_t)idx, CAL_N,
                         CAL_NE, CAL_NH, 0, CAL_EDGE_STEP, 0, CAL_HOLD_STEP, saved };
   for (uint32_t i = 0; i < sizeof(h); i++) {
      if (!cal_putc(h[i], i ? CAL_RESULT_US : CAL_HEADER_US))
         return false;
   }
   for (uint32_t i = 0; i < CAL_N; i++) {
      if (!cal_putc(cal_map[i], CAL_RESULT_US))
         return false;
   }
   return true;
}

// one run of the test program with the given timing (idx: grid point shown
// as under test, CAL_N if none): true if no error
static bool cal_run_values(core1_args_t *args, uint32_t edge, uint32_t hold, uint32_t idx) {
   cal_start(args, edge, hold);
   uint8_t r[4];
   bool ok = cal_header(0, edge, hold, idx, false);
   for (uint32_t i = 0; ok && i < sizeof(r); i++)
      ok = cal_getc(&r[i], CAL_RESULT_US);
   bool pass = ok && r[0] == 'R' && r[1] == 0 && r[2] == 0;
   if (!ok)
      printf("cal: edge %2lu hold %2lu: no answer (crash)\n", (unsigned long)edge, (unsigned long)hold);
   else if (!pass)
      printf("cal: edge %2lu hold %2lu: %u errors, flags $%02X\n", (unsigned long)edge,
             (unsigned long)hold, r[1] | r[2] << 8, r[3]);
   return pass;
}

static inline uint32_t cal_edge(uint32_t idx) { return (idx % CAL_NE) * CAL_EDGE_STEP; }
static inline uint32_t cal_hold(uint32_t idx) { return (idx / CAL_NE) * CAL_HOLD_STEP; }

static bool cal_run(core1_args_t *args, uint32_t idx) {
   return cal_run_values(args, cal_edge(idx), cal_hold(idx), idx);
}

// the pair must pass CAL_CONFIRM more runs
static bool cal_confirm(core1_args_t *args, uint32_t edge, uint32_t hold) {
   for (int k = 0; k < CAL_CONFIRM; k++) {
      if (!cal_run_values(args, edge, hold, CAL_N))
         return false;
   }
   return true;
}

// passing grid point nearest to (edge, hold) in cycles, -1 if none
static int cal_nearest(uint32_t edge, uint32_t hold) {
   int best = -1;
   uint32_t best_d = 0;
   for (uint32_t i = 0; i < CAL_N; i++) {
      if (cal_map[i] != CAL_PASS)
         continue;
      int de = (int)cal_edge(i) - (int)edge, dh = (int)cal_hold(i) - (int)hold;
      uint32_t d = de * de + dh * dh;
      if (best < 0 || d < best_d) {
         best = i;
         best_d = d;
      }
   }
   return best;
}

static void cal_print_map(void) {
   printf("cal: map (O pass, X fail), rows hold 0-%d, columns edge 0-%d step %d\n",
          CAL_HOLD_MAX, CAL_EDGE_MAX, CAL_EDGE_STEP);
   for (uint32_t h = 0; h < CAL_NH; h++) {
      printf("  %2lu ", (unsigned long)(h * CAL_HOLD_STEP));
      for (uint32_t e = 0; e < CAL_NE; e++)
         printf(" %c", "-OX"[cal_map[h * CAL_NE + e]]);
      printf("\n");
   }
}

void bus_calibrate(core1_args_t *args) {

   uint32_t old_edge = c64_bus_edge_cycles, old_hold = c64_bus_hold_cycles;
   printf("cal: bus timing calibration, %d pairs (current edge %lu, hold %lu)\n", CAL_N,
          (unsigned long)old_edge, (unsigned long)old_hold);

   // row by row (hold), edge increasing: short edge delays may fail (select
   // line glitches not masked), then a working range, then failures again (a
   // longer edge delay only leaves less time to serve the cycle). After
   // CAL_SKIP_AFTER failures following a pass the rest of the row fails
   // without a run (a crash costs CAL_RESULT_US)
   memset(cal_map, CAL_UNTESTED, CAL_N);
   for (uint32_t h = 0; h < CAL_NH; h++) {
      uint32_t fails = 0;
      bool passed = false;
      for (uint32_t e = 0; e < CAL_NE; e++) {
         uint32_t i = h * CAL_NE + e;
         bool pass = !(passed && fails >= CAL_SKIP_AFTER) && cal_run(args, i);
         cal_map[i] = pass ? CAL_PASS : CAL_FAIL;
         fails = pass ? 0 : fails + 1;
         passed = passed || pass;
      }
   }
   cal_print_map();

   // the mean of the passing pairs (in the middle of the working area), if
   // it passes CAL_CONFIRM more runs, else the nearest passing grid point
   // that does (a failing one is marked and the next nearest is tried)
   uint32_t n = 0, sum_e = 0, sum_h = 0;
   for (uint32_t i = 0; i < CAL_N; i++) {
      if (cal_map[i] == CAL_PASS) {
         n++;
         sum_e += cal_edge(i);
         sum_h += cal_hold(i);
      }
   }
   bool found = false;
   uint32_t edge = 0, hold = 0;
   if (n) {
      edge = (sum_e + n / 2) / n;
      hold = (sum_h + n / 2) / n;
      printf("cal: %lu passing pairs, mean edge %lu, hold %lu\n", (unsigned long)n,
             (unsigned long)edge, (unsigned long)hold);
      found = cal_confirm(args, edge, hold);
      int i;
      uint32_t me = edge, mh = hold;
      while (!found && (i = cal_nearest(me, mh)) >= 0) {
         edge = cal_edge(i);
         hold = cal_hold(i);
         found = cal_confirm(args, edge, hold);
         if (!found)
            cal_map[i] = CAL_FAIL;
      }
   }

   uint8_t mode;
   bool saved = false;
   if (found) {
      c64_bus_edge_cycles = edge;
      c64_bus_hold_cycles = hold;
      saved = config_save();
      mode = 1;
      printf("cal: chosen edge %lu, hold %lu cycles, %s\n", (unsigned long)edge,
             (unsigned long)hold, saved ? "saved" : "NOT saved");
   } else {
      edge = old_edge;
      hold = old_hold;
      mode = 2;
      printf("cal: no working pair, timing unchanged\n");
   }

   // result on the C64 screen (the test program stays there)
   c64_bus_edge_cycles = edge;
   c64_bus_hold_cycles = hold;
   cal_start(args, edge, hold);
   cal_header(mode, edge, hold, CAL_N, saved);
}

#ifdef DEBUG
/*
 * Edge delay scan with a real EasyFlash cartridge (shell "bus scan"): the
 * calibration test program may not show the lower limit of the edge delay
 * (a game that crashes with a short edge delay, while the test passes).
 * The cartridge is started scan_repeat times per edge delay value (hold time
 * unchanged) and runs for scan_seconds; a crash (JAM or BRK, see
 * BusCrashDetect) ends that run. A crash may be random: the summary gives
 * the crashes per value. With the cartridge in the PSRAM, the time from the
 * last cache miss is printed with each crash (a crash right after a miss
 * points to the DMA pause, not to the edge delay). EAPI is served (no save
 * file: flash writes stay in memory for the next runs). Any key on the
 * console stops the scan. The timing is not changed: the result is a
 * suggestion (bus edge <n> save).
 */
#define SCAN_MAX_VALUES   32
static uint32_t scan_seconds;           // scan requested: seconds per run, 0 = none
static uint32_t scan_from, scan_to, scan_step, scan_repeat;
static bool scan_running;               // no cache miss messages from cart_poll
volatile uint32_t bus_scan_crash;       // set by BusCrashDetect (core1)

bool bus_scan_pending(void) {
   return scan_seconds != 0;
}

bool bus_scan_running(void) {
   return scan_running;
}

void bus_scan_request(uint32_t seconds, uint32_t from, uint32_t to, uint32_t step, uint32_t repeat) {
   scan_seconds = seconds;
   scan_from = from;
   scan_to = to;
   scan_step = step ? step : 1;
   scan_repeat = repeat ? repeat : 1;
}

void bus_scan_ef(core1_args_t *args, bool exrom, bool game) {

   uint32_t seconds = scan_seconds;
   scan_seconds = 0;                       // one scan per request
   uint32_t old_edge = c64_bus_edge_cycles;
   uint32_t nvalues = (scan_to - scan_from) / scan_step + 1;
   if (nvalues > SCAN_MAX_VALUES)
      nvalues = SCAN_MAX_VALUES;
   uint8_t crashes[SCAN_MAX_VALUES] = {};
   uint8_t runs[SCAN_MAX_VALUES] = {};
   printf("scan: edge %lu-%lu step %lu, hold %lu, %lu runs of %lu s per value (any key stops)\n",
          (unsigned long)scan_from, (unsigned long)(scan_from + (nvalues - 1) * scan_step),
          (unsigned long)scan_step, (unsigned long)c64_bus_hold_cycles,
          (unsigned long)scan_repeat, (unsigned long)seconds);

   scan_running = true;
   eapi_start();
   bool stopped = false;
   for (uint32_t k = 0; k < nvalues && !stopped; k++) {
      uint32_t e = scan_from + k * scan_step;
      for (uint32_t r = 0; r < scan_repeat && !stopped; r++) {
         multicore_reset_core1();
         c64_dma_bus_restore();
         c64_dma_release();
         c64_hold_reset();
         ef3_rx_rdy = EF3_NOT_RDY;
         ef3_tx_rdy = EF3_TX_RDY;
         c64_bus_edge_cycles = e;
         bus_scan_crash = 0;
         uint32_t misses0 = ef_misses;
         c64_set_exrom_game(exrom, game);
         multicore_launch_core1(run_cart_easyflash_scan);
         multicore_fifo_push_blocking((uint32_t)args);

         uint64_t t0 = time_us_64();
         uint32_t crash = 0;
         while (time_us_64() - t0 < (uint64_t)seconds * 1000000) {
            if ((crash = bus_scan_crash) != 0)
               break;
            cart_poll();                    // EAPI
            if (getchar_timeout_us(1000) != PICO_ERROR_TIMEOUT) {
               stopped = true;
               break;
            }
         }
         if (stopped)
            break;
         uint32_t misses = ef_misses - misses0;
         runs[k]++;
         if (crash) {
            crashes[k]++;
            printf("scan: edge %2lu run %lu: CRASH (%s) after %lu ms, %lu cache misses",
                   (unsigned long)e, (unsigned long)r + 1, crash == 1 ? "JAM" : "BRK",
                   (unsigned long)((time_us_64() - t0) / 1000), (unsigned long)misses);
            if (misses)
               printf(", last %lu ms before",
                      (unsigned long)((timer_hw->timerawl - ef_last_miss_us) / 1000));
            printf("\n");
         } else {
            printf("scan: edge %2lu run %lu: ok, %lu cache misses\n", (unsigned long)e,
                   (unsigned long)r + 1, (unsigned long)misses);
         }
      }
   }
   scan_running = false;
   if (stopped)
      printf("scan: stopped\n");

   // summary: crashes per value, lowest value from which no run crashed
   int32_t low_ok = -1;
   printf("scan: edge  crashes/runs\n");
   for (uint32_t k = 0; k < nvalues; k++) {
      if (!runs[k])
         continue;
      uint32_t e = scan_from + k * scan_step;
      printf("scan:  %2lu   %u/%u\n", (unsigned long)e, crashes[k], runs[k]);
      if (crashes[k])
         low_ok = -1;
      else if (low_ok < 0)
         low_ok = e;
   }
   if (low_ok >= 0)
      printf("scan: no crash from edge %ld up\n", (long)low_ok);
   else
      printf("scan: the highest edge crashed too\n");

   // the cartridge again with the configured timing
   multicore_reset_core1();
   c64_dma_bus_restore();
   c64_dma_release();
   c64_hold_reset();
   ef3_rx_rdy = EF3_NOT_RDY;
   ef3_tx_rdy = EF3_TX_RDY;
   c64_bus_edge_cycles = old_edge;
   c64_set_exrom_game(exrom, game);
   multicore_launch_core1(run_cart_easyflash);
   multicore_fifo_push_blocking((uint32_t)args);
   printf("scan: cartridge restarted with edge %lu\n", (unsigned long)old_edge);
}
#endif

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
/*
 * EasyFlash CPU trace: state (recorded by cpu_trace_record() in bus_diag.h)
 */
volatile bool cpu_trace_on;
volatile bool cpu_trace_frozen;
volatile uint32_t cpu_trace_head;
volatile uint32_t cpu_trace_post;
volatile uint32_t cpu_trace_trig_at;
volatile uint32_t cpu_trace_trig_addr[CPU_TRACE_TRIGGERS] = { 0xFE66, 0xEA31 };
volatile uint32_t cpu_trace_trig_data[CPU_TRACE_TRIGGERS] = { 0x100, 0x00 };
volatile bool cpu_trace_trig_write[CPU_TRACE_TRIGGERS] = { false, false };
uint32_t cpu_trace_last;
uint32_t cpu_trace_repeat;
volatile bool cpu_trace_brk = true;

void cpu_trace_reset(bool available) {
   cpu_trace_on = false;
   cpu_trace_head = 0;
   cpu_trace_post = 0;
   cpu_trace_trig_at = 0xFFFFFFFF;
   cpu_trace_last = 0;
   cpu_trace_repeat = 0;
   cpu_trace_frozen = false;
   cpu_trace_on = available;
}
#endif

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
/*
 * Shell "cputrace" command (EasyFlash CPU trace, see EF_DEBUG_CPU_TRACE)
 *   cputrace              status
 *   cputrace dump [n]     print the last n cycles (default 2000), oldest first
 *   cputrace trig [w]<addr>[=<data>] [[w]<addr>[=<data>]]
 *                         triggers (hex): read of an address (write with the
 *                         "w" prefix), optionally only when the data is
 *                         <data>; "0" = none (stop manually).
 *                         Default: FE66 EA31=00
 *   cputrace brk on|off   BRK trigger (default on)
 *   cputrace stop         stop recording now
 *   cputrace run          clear and record again
 * Line format: cycle (relative to the trigger or to the last one), address,
 * R/W, data, then the asserted lines (ROML ROMH IO1 IO2), "ba" when the
 * VIC-II requests the bus (BA low) and "OUT" when the cartridge drove the
 * data bus in that cycle.
 */
static void cpu_trace_print_triggers(void) {
   printf("cputrace: triggers");
   bool any = false;
   for (int i = 0; i < CPU_TRACE_TRIGGERS; i++) {
      if (!cpu_trace_trig_addr[i])
         continue;
      any = true;
      printf(" %s$%04lX", cpu_trace_trig_write[i] ? "write " : "", (unsigned long)cpu_trace_trig_addr[i]);
      if (cpu_trace_trig_data[i] <= 0xFF)
         printf("=$%02lX", (unsigned long)cpu_trace_trig_data[i]);
   }
   printf("%s\n", any ? "" : " none");
}

void cpu_trace_shell(char *args) {

   char *token = args ? strtok(args, " ") : NULL;

   if (token && strcmp(token, "trig") == 0) {
      // up to CPU_TRACE_TRIGGERS "<addr>[=<data>]" (hex), "0" = none
      char *t[CPU_TRACE_TRIGGERS];
      int n = 0;
      while (n < CPU_TRACE_TRIGGERS && (t[n] = strtok(NULL, " ")) != NULL)
         n++;
      if (n) {
         for (int i = 0; i < CPU_TRACE_TRIGGERS; i++) {
            uint32_t addr = 0, data = 0x100;
            bool write = false;
            if (i < n) {
               char *p = t[i], *end;
               if (*p == 'w' || *p == 'W') {
                  write = true;
                  p++;
               }
               addr = strtoul(p, &end, 16) & 0xFFFF;
               if (*end == '=')
                  data = strtoul(end + 1, NULL, 16) & 0xFF;
            }
            cpu_trace_trig_addr[i] = addr;
            cpu_trace_trig_data[i] = data;
            cpu_trace_trig_write[i] = write;
         }
      }
      cpu_trace_print_triggers();
      return;
   }

   if (token && strcmp(token, "brk") == 0) {
      token = strtok(NULL, " ");
      if (token)
         cpu_trace_brk = strcmp(token, "off") != 0;
      printf("cputrace: BRK trigger %s\n", cpu_trace_brk ? "on" : "off");
      return;
   }

   if (token && strcmp(token, "stop") == 0) {
      cpu_trace_frozen = true;
      printf("cputrace: stopped\n");
      return;
   }

   if (token && strcmp(token, "run") == 0) {
      bool on = cpu_trace_on;
      cpu_trace_frozen = true;
      cpu_trace_reset(on);
      printf("cputrace: %s\n", on ? "recording" : "not available");
      return;
   }

   uint32_t head = cpu_trace_head;
   uint32_t avail = head < CPU_TRACE_ENTRIES ? head : CPU_TRACE_ENTRIES;

   if (!token || strcmp(token, "dump") != 0) {
      printf("cputrace: %s, %s, %lu cycles recorded, trigger %s\n",
             cpu_trace_on ? "available" : "not available",
             cpu_trace_frozen ? "stopped" : "recording", (unsigned long)avail,
             cpu_trace_trig_at != 0xFFFFFFFF ? "hit" : "not hit");
      cpu_trace_print_triggers();
      return;
   }

   if (!cpu_trace_frozen) {
      printf("cputrace: stopped for the dump\n");
      cpu_trace_frozen = true;
      head = cpu_trace_head;
      avail = head < CPU_TRACE_ENTRIES ? head : CPU_TRACE_ENTRIES;
   }

   uint32_t n = 2000;
   token = strtok(NULL, " ");
   if (token)
      n = strtoul(token, NULL, 10);
   if (n > avail)
      n = avail;

   uint32_t ref = (cpu_trace_trig_at != 0xFFFFFFFF) ? cpu_trace_trig_at : head - 1;

   for (uint32_t i = head - n; i != head; i++) {
      uint32_t c = cpu_trace_buf[i & (CPU_TRACE_ENTRIES - 1)];
      printf("%+6ld %04lX %c %02X%s%s%s%s%s%s\n", (long)(i - ref),
             (unsigned long)bus_addr(c), (c & RW_MASK) ? 'r' : 'W', bus_data(c),
             (c & ROML_MASK) ? "" : " ROML", (c & ROMH_MASK) ? "" : " ROMH",
             (c & IO1_MASK) ? "" : " IO1", (c & IO2_MASK) ? "" : " IO2",
             (c & BA_MASK) ? "" : " ba", (c & (1u << 31)) ? " OUT" : "");
   }
}
#else
void cpu_trace_shell(char *args) {
   (void)args;
   printf("cputrace: not enabled (EF_DEBUG_CPU_TRACE in bus_diag.h)\n");
}
#endif
