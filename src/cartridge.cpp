#include <stdio.h>      // printf
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "cartridge.h"
#include "c64_interface.h"
#include "c64_bus.h"

#include "board.h"
#include "commands.h"
#include "filesystem.h"
#include "ef_save.h"
#include <string.h>
#include <stdlib.h>     // strtoul

uint8_t crt_buf[CRT_BUFFER_SIZE] = {};
uint8_t crt_map[EF_MAP_SIZE] = {};

// EasyFlash bank -> 8K chunk pointers (built by run_cart, read by core1)
static uint8_t *ef_roml[EF_BANKS];
static uint8_t *ef_romh[EF_BANKS];

// EasyFlash: first free 8K chunk and erased chunk (used by EAPI writes)
static uint8_t *ef_blank;
static uint32_t ef_next_chunk;

// identity of the EasyFlash cartridge data, checked by the save file
static uint32_t ef_crt_hash;

// EasyFlash bank register, published by core1 for EAPI commands on core0
static volatile uint8_t ef_cur_bank;

// EAPI commands are served by cart_poll() on core0
static bool eapi_active = false;

/*
 * EF_DEBUG_TRACE (needs a DEBUG build)
 *   1: trace of EasyFlash register writes ($DE00/$DE02), recorded by core1
 *      and printed on the console by cart_poll()
 *   0: disabled
 */
#ifndef EF_DEBUG_TRACE
#define EF_DEBUG_TRACE   0
#endif

#if defined(DEBUG) && EF_DEBUG_TRACE
#define EF_TRACE_SIZE   64
static volatile uint16_t ef_trace[EF_TRACE_SIZE];   // reg << 8 | data
static volatile uint32_t ef_trace_head;
#define EF_TRACE(reg, data) do { \
      ef_trace[ef_trace_head & (EF_TRACE_SIZE - 1)] = ((reg) << 8) | (data); \
      ef_trace_head = ef_trace_head + 1; \
   } while (0)
#else
#define EF_TRACE(reg, data) do { } while (0)
#endif

/*
 * EF_DEBUG_CPU_TRACE (needs a DEBUG build)
 *   1: every C64 cycle of a running EasyFlash cartridge (address, R/W, data,
 *      select lines, BA) is recorded by core1 in a ring buffer at the end of
 *      crt_buf, like a logic analyzer. Recording stops CPU_TRACE_POST cycles
 *      after the CPU reads a trigger address outside the cartridge ROM
 *      (defaults: see cpu_trace_trig_addr). Consecutive identical cycles
 *      are recorded once (a JAM does not overwrite the history).
 *      Shell command: cputrace [dump [n] | trig [w]<addr>[=<data>] ... | stop | run]
 *   0: disabled
 */
#ifndef EF_DEBUG_CPU_TRACE
#define EF_DEBUG_CPU_TRACE   0
#endif

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
#define CPU_TRACE_ENTRIES   (32 * 1024)                 // 128K, power of 2
#define CPU_TRACE_BYTES     (CPU_TRACE_ENTRIES * 4)
#define CPU_TRACE_POST      2048                         // cycles kept after the trigger
#define CPU_TRACE_TRIGGERS  2

static uint32_t * const cpu_trace_buf = (uint32_t *)(crt_buf + CRT_BUFFER_SIZE - CPU_TRACE_BYTES);
static volatile bool cpu_trace_on;          // buffer available (cartridge fits)
static volatile bool cpu_trace_frozen;
static volatile uint32_t cpu_trace_head;     // entries written
static volatile uint32_t cpu_trace_post;     // entries left after the trigger, 0 = not triggered
static volatile uint32_t cpu_trace_trig_at;  // entry index of the trigger
// triggers: CPU read of an address, optionally with a given data (0x100 = any)
// defaults: $FE66 KERNAL BRK handler (crash -> "READY"),
//           $EA31 = $00 KERNAL IRQ entry read from RAM, i.e. a BRK (crash
//           with the KERNAL banked out -> black screen)
static volatile uint32_t cpu_trace_trig_addr[CPU_TRACE_TRIGGERS] = { 0xFE66, 0xEA31 };
static volatile uint32_t cpu_trace_trig_data[CPU_TRACE_TRIGGERS] = { 0x100, 0x00 };
static volatile bool cpu_trace_trig_write[CPU_TRACE_TRIGGERS] = { false, false };   // write instead of read
static uint32_t cpu_trace_last;

static void cpu_trace_reset(bool available) {
   cpu_trace_on = false;
   cpu_trace_head = 0;
   cpu_trace_post = 0;
   cpu_trace_trig_at = 0xFFFFFFFF;
   cpu_trace_last = 0;
   cpu_trace_frozen = false;
   cpu_trace_on = available;
}

// core1, once per cycle
static inline __attribute__((always_inline)) void cpu_trace_record(uint32_t control) {

   if (!cpu_trace_on || cpu_trace_frozen || control == cpu_trace_last)
      return;

   cpu_trace_last = control;
   uint32_t head = cpu_trace_head;
   cpu_trace_buf[head & (CPU_TRACE_ENTRIES - 1)] = control;
   cpu_trace_head = head + 1;

   uint32_t post = cpu_trace_post;
   if (post) {
      if (--post == 0)
         cpu_trace_frozen = true;
      cpu_trace_post = post;
   } else if (!bus_rom_sel(control)) {
      // reads of the cartridge ROM do not trigger (e.g. the EasyFlash boot
      // code copies $FE00-$FEFF from ROMH in Ultimax mode)
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
#define EF_BUF_LIMIT   (CRT_BUFFER_SIZE - (cpu_trace_on ? CPU_TRACE_BYTES : 0))
#else
#define EF_BUF_LIMIT   CRT_BUFFER_SIZE
#endif


/*
 * EF3 USB registers ($DE09 status, $DE0A data), used by:
 *  - the launcher in EF3 mode, to load PRG files
 *  - the Kung Fu Flash EAPI installed in EasyFlash cartridges
 */
#define EF3_RX_RDY   0x80
#define EF3_TX_RDY   0x40
#define EF3_NOT_RDY  0x00

// shared between core0 (ef3_putc/ef3_getc) and core1 (bus loop)
static volatile uint8_t ef3_rx_rdy = EF3_NOT_RDY;   // data for C64 available
static volatile uint8_t ef3_tx_rdy = EF3_TX_RDY;    // C64 may send data
static volatile uint8_t ef3_rx_data;
static volatile uint8_t ef3_tx_data;
static uint8_t ef3_ram[256];

// last bytes received from the C64, to recognize "KFF:" EAPI commands (cart_poll)
static uint8_t eapi_window[5];

/*
 * Stop the running cartridge before another one is started (launcher, CRT,
 * ROM, PRG): core1 is reset, the C64 is held in reset with the cartridge
 * disconnected while the new file is loaded (the new core1 handler releases
 * it with c64_reset()), and the state shared between the cores is cleared.
 * The handler state (EasyFlash RAM, bank, locks...) is local to core1 and is
 * created again by every handler.
 */
// C64 clean start before every cartridge/program (see run_c64_clean)
static bool c64_clean_enabled = true;

void c64_clean_enable(bool enable) {
   c64_clean_enabled = enable;
}

bool c64_clean_is_enabled(void) {
   return c64_clean_enabled;
}

// power-on like C64 state before a cartridge/program is started, if enabled
static void c64_clean_before_start(void) {
   if (c64_clean_enabled)
      run_c64_clean();
}

// ROM select lines of a cartridge mode (same arguments as c64_set_exrom_game)
static uint32_t cart_rom_mask(bool exrom, bool game) {
   if (!exrom)
      return game ? (uint32_t)ROML_MASK : (uint32_t)(ROML_MASK | ROMH_MASK);   // 8K / 16K
   return game ? 0 : (uint32_t)(ROML_MASK | ROMH_MASK);                       // off / Ultimax
}

static void cart_stop(void) {

   // write a pending EasyFlash save before crt_buf is reused
   ef_save_stop();

   multicore_reset_core1();

   c64_hold_reset();
   c64_set_exrom_game(1, 1);

   eapi_active = false;
#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
   cpu_trace_reset(false);
#endif
   ef3_rx_rdy = EF3_NOT_RDY;
   ef3_tx_rdy = EF3_TX_RDY;
   memset(eapi_window, 0, sizeof(eapi_window));
}
volatile uint8_t kff_ram[256] = {};

// Fast look-up of 16k ROM bank address
uint8_t *crt_banks[BANKS_NUM] = {
   CRT_BANK(0),
   CRT_BANK(1),
   CRT_BANK(2),
   CRT_BANK(3),
   CRT_BANK(4),
   CRT_BANK(5),
   CRT_BANK(6),
   CRT_BANK(7),
   CRT_BANK(8),
   CRT_BANK(9),
   CRT_BANK(10),
   CRT_BANK(11),
   CRT_BANK(12),
   CRT_BANK(13),
   CRT_BANK(14),
   CRT_BANK(15),
   CRT_BANK(16),
   CRT_BANK(17),
   CRT_BANK(18),
   CRT_BANK(19),
   CRT_BANK(20),
   CRT_BANK(21),
   CRT_BANK(22),
   CRT_BANK(23),
   CRT_BANK(24),
   CRT_BANK(25),
   CRT_BANK(26),
   CRT_BANK(27),
   CRT_BANK(28),
   CRT_BANK(29)
};

void kff_init(void);

static bool _launcher_state = false;

void launcher_enable(void) {
   printf("launcher: ON\n");
   _launcher_state = true;
}

void launcher_disable(void) {
   printf("launcher: OFF\n");
   _launcher_state = false;
}

bool launcher_running(void) {
   return _launcher_state;
}

uint8_t run_launcher(void) {

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   cart_stop();

   // init cart memory
   memset(crt_buf, 0, sizeof(crt_buf));
   memset(crt_map, 0, sizeof(crt_map));
   for(uint16_t i=0; i<sizeof(kff_ram); i++)
      kff_ram[i] = 0;

   memcpy(crt_buf, CRT_LAUNCHER, CRT_LAUNCHER_SIZE);

   printf("\ncart: Launcher\n");
   kff_init();
   c64_set_exrom_game(1, 0);
   args = {};
   args.crt_buf = crt_buf;
   launcher_enable();
   multicore_launch_core1(run_cart_kff);
   multicore_fifo_push_blocking((uint32_t)&args);

   printf("done\n");
   return 0;
}

/*
 * Run a raw cartridge image (.ROM/.BIN, no CRT header), like Kung Fu Flash:
 *  - "CBM80" at $8004: normal cartridge, 8K if the image is <= 8K, else 16K
 *  - otherwise Ultimax: 4K/8K images are mirrored up to 16K and the reset
 *    vector must point to cartridge ROM
 * rom must hold at least 16K, padded with $FF after len bytes; it may be inside
 * crt_buf (above the first 16K). The image is validated before the launcher
 * is stopped, so on error the launcher can still show a message.
 */
uint8_t run_rom(uint8_t *rom, uint32_t len) {

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   // C128 cartridge identifier "CBM" at $8007 or $C007
   if (len && (memcmp("CBM", rom + 0x0007, 3) == 0 || memcmp("CBM", rom + 0x4007, 3) == 0))
      return ROM_C128;

   if (!len || len > 16*1024)
      return ROM_UNSUPPORTED;

   bool exrom, game;

   // C64 cartridge identifier "CBM80" (PETSCII)
   if (memcmp("\xc3\xc2\xcd" "80", rom + 0x0004, 5) == 0) {
      exrom = 0;
      game = (len <= 8*1024);     // 8K: GAME high, 16K: GAME low
   } else {
      // assume Ultimax
      exrom = 1;
      game = 0;
      if (len <= 4*1024)
         memcpy(rom + 4*1024, rom, 4*1024);
      if (len <= 8*1024)
         memcpy(rom + 8*1024, rom, 8*1024);

      // reset vector must point to ROML ($8000-$9FFF) or ROMH ($E000-$FFFF)
      uint8_t reset_hi = rom[0x3FFD];
      if (reset_hi < 0x80 || (reset_hi >= 0xA0 && reset_hi < 0xE0))
         return ROM_UNSUPPORTED;
   }

   printf("\nrom: %lu bytes, %s\n", (unsigned long)len,
          exrom ? "Ultimax" : (game ? "8K" : "16K"));

   c64_clean_before_start();
   cart_stop();

   // the launcher ROM copy in crt_buf is replaced by the cartridge image
   memcpy(crt_buf, rom, 16*1024);

   // exit from launcher
   launcher_disable();

   c64_set_exrom_game(exrom, game);
   args = {};
   args.crt_buf = crt_buf;
   args.ultimax = exrom && !game;
   args.rom_mask = cart_rom_mask(exrom, game);
   multicore_launch_core1(run_cart_normal);
   multicore_fifo_push_blocking((uint32_t)&args);

   printf("done\n");
   return ROM_OK;
}

// mask of 8K bank number bits decoded by a cartridge of the given size
// (size rounded up to a power of two, like real ROM chips)
static uint32_t cart_bank_mask(uint32_t size) {
   uint32_t banks = (size + 8*1024 - 1) / (8*1024);
   uint32_t mask = 1;
   while (mask < banks)
      mask <<= 1;
   return mask - 1;
}

// EAPI driver: $F800-$FAFF in bank 0 ROMH, $B800 in the 16K launcher image
#define EAPI_OFFSET_ROMH       0x1800
#define EAPI_OFFSET_LAUNCHER   0x3800
#define EAPI_SIZE              0x300

// EasyFlash: resolve the chunk map built by CRTParser into pointer tables.
// Missing chips point to an erased (0xFF) chunk, like an empty flash.
static void easyflash_build_tables(uint8_t chunks) {

   uint8_t *blank;

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
   // the CPU trace uses the end of crt_buf, if the cartridge leaves it free
   cpu_trace_reset((uint32_t)(chunks + 1) * EF_CHUNK_SIZE <= CRT_BUFFER_SIZE - CPU_TRACE_BYTES);
   printf("EasyFlash: CPU trace %s\n", cpu_trace_on ? "on" : "off (cartridge too large)");
#endif

   if ((uint32_t)(chunks + 1) * EF_CHUNK_SIZE <= EF_BUF_LIMIT) {
      blank = crt_buf + (uint32_t)chunks * EF_CHUNK_SIZE;
      memset(blank, 0xFF, EF_CHUNK_SIZE);
      ef_next_chunk = chunks + 1;
   } else {
      // buffer full: no room for an erased chunk
      blank = crt_buf;
      ef_next_chunk = chunks;
   }
   ef_blank = blank;
   ef_cur_bank = 0;

   for (int bank = 0; bank < EF_BANKS; bank++) {
      uint8_t l = crt_map[bank];
      uint8_t h = crt_map[EF_BANKS + bank];
      ef_roml[bank] = (l == EF_CHUNK_NONE) ? blank : crt_buf + (uint32_t)l * EF_CHUNK_SIZE;
      ef_romh[bank] = (h == EF_CHUNK_NONE) ? blank : crt_buf + (uint32_t)h * EF_CHUNK_SIZE;
   }

   printf("EasyFlash: %d chunks (8K) used\n", chunks);

   // identity of the original cartridge data, checked by the save file
   ef_crt_hash = ef_save_hash(crt_buf, (uint32_t)chunks * EF_CHUNK_SIZE);

   // replace the EAPI flash driver in bank 0 ROMH ($F800) with the
   // Kung Fu Flash one from the launcher, that talks to cart_poll()
   uint8_t *eapi = ef_romh[0] + EAPI_OFFSET_ROMH;
   if (ef_romh[0] != blank && memcmp(eapi, "eapi", 4) == 0 &&
       memcmp(CRT_LAUNCHER + EAPI_OFFSET_LAUNCHER, "eapi", 4) == 0) {
      memcpy(eapi, CRT_LAUNCHER + EAPI_OFFSET_LAUNCHER, EAPI_SIZE);
      printf("EasyFlash: EAPI replaced\n");
   } else {
      // writes to the flash are only supported through the EAPI
      printf("EasyFlash: no EAPI found, flash writes not supported\n");
   }
}

uint8_t run_cart(IDataReader &r, const char *file_name) {

   c64_clean_before_start();
   cart_stop();

   // init cart memory
   memset(crt_buf, 0, sizeof(crt_buf));
   memset(crt_map, 0, sizeof(crt_map));

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   CRTParser *crt = new CRTParser(r, crt_buf, sizeof(crt_buf), crt_map, sizeof(crt_map));
   if (crt->parse() != true) {
      printf("E: CRT parsing error\n");
      delete crt;
      c64_release_reset();      // no cartridge: the C64 starts BASIC
      return 1;
   }

   args = {};
   args.ultimax = crt->getExrom() && !crt->getGame();
   args.bank_mask = cart_bank_mask(crt->getSize());
   args.rom_mask = cart_rom_mask(crt->getExrom(), crt->getGame());

   printf("\nname: %s\n", crt->getName());
   printf("EXROM: %d, GAME: %d\n", crt->getExrom(), crt->getGame());
   c64_set_exrom_game(crt->getExrom(), crt->getGame());
   printf("CRT size: %ld\n", crt->getSize());

   // exit from launcher
   launcher_disable();

   switch(crt->getType()) {

      case 0:
         // normal cartridge (0)
         printf("cart: 8K, 16K, Ultimax\n");
         args.crt_buf = crt_buf;
         multicore_launch_core1(run_cart_normal);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 5:
         // Ocean (5)
         args.crt_buf = crt_buf;
         printf("cart: Ocean\n");
         multicore_launch_core1(run_cart_ocean);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 7:
         // Fun Play (7)
         args.crt_banks = crt_banks;
         printf("cart: Fun Play\n");
         multicore_launch_core1(run_cart_fun_play);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 8:
         // Super Games (8)
         args.crt_banks = crt_banks;
         printf("cart: Super Games\n");
         multicore_launch_core1(run_cart_super_games);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 17:
         // Dinamic (17)
         args.crt_banks = crt_banks;
         printf("cart: Dinamic\n");
         multicore_launch_core1(run_cart_dinamic);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 18:
         // Zaxxon (18)
         args.crt_banks = crt_banks;
         printf("cart: Zaxxon\n");
         multicore_launch_core1(run_cart_zaxxon);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 19:
         // Magic Desk (19)
         printf("cart: Magic Desk\n");
         args.crt_banks = crt_banks;
         multicore_launch_core1(run_cart_magic_desk);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 32:
         // EasyFlash (32)
         easyflash_build_tables(crt->getEasyFlashChunks());
         ef_save_start(file_name, crt->getSize(), ef_crt_hash);   // apply "<name>.sav", if any
         eapi_active = true;
         args.crt_banks = crt_banks;
         args.crt_map = crt_map;
         printf("cart: EasyFlash\n");
         multicore_launch_core1(run_cart_easyflash);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      default:
         printf("E: cartridge type %d - not found\n", crt->getType());
         break;

   } // end switch

   delete crt;

   printf("done\n");
   return 0;
}

//

/*
 * Cartridge handlers
 *
 * Each cartridge type is described by a small struct (see c64_bus.h for the
 * interface) and driven by the common bus loop c64_bus_loop().
 */

#define CART_FUNC    __attribute__((optimize("O3"), hot))
#define CART_METHOD  inline __attribute__((always_inline))

/*
 * ROM select lines answered by a handler (rom_mask)
 *
 * A handler drives the data bus on ROML/ROMH only when the current cartridge
 * mode maps them: a glitch of ROML/ROMH while the cartridge is off (or of
 * ROMH in 8K mode) must not drive the bus. Seen on a breadbin with a GAL PLA:
 * a real ROM/flash ignores pulses shorter than its output enable time, while
 * the bus loop samples every few tens of ns (random bit errors in RAM while
 * the program runs with the cartridge off).
 */
#define ROM_MASK_16K       (ROML_MASK | ROMH_MASK)
#define ROM_MASK_8K        ROML_MASK
#define ROM_MASK_ULTIMAX   (ROML_MASK | ROMH_MASK)
#define ROM_MASK_OFF       0

// true if one of the enabled ROM select lines is asserted
#define ROM_SEL(control, mask)   ((~(control)) & (mask))

/*
 * Normal cartridge: 8K, 16K, Ultimax
 */
struct NormalCart {

   static constexpr uint32_t COMMIT_SEL = 0;   // no registers

   uint8_t *rom;
   bool ultimax;
   uint32_t rom_mask;      // fixed by the CRT header (8K, 16K, Ultimax)

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) { (void)control; }

   CART_METHOD bool vic_enabled(void) { return ultimax; }

   CART_METHOD uint8_t vic_read(uint32_t addr) {
      return rom[(addr & 0x1FFF) | 0x2000];
   }
};

CART_FUNC void __time_critical_func(run_cart_normal)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   NormalCart cart = { p->crt_buf, p->ultimax, p->rom_mask };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Magic Desk (19)
 *
 * $DE00 write: bit 0-6 = 8K bank (ROML), bit 7 = 1 cartridge disabled
 * 8K banks are stored in 16K slots: even banks at ROML offset, odd banks at ROMH offset
 */
struct MagicDeskCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t **banks;
   uint8_t *rom_ptr;
   uint32_t bank_mask;
   uint32_t rom_mask;      // ROML when on, none when off

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      if ((control & RW_MASK) || (bus_addr(control) & 0xFF))
         return;

      uint8_t data = bus_data(control);

      if (!(data & 0x80)) {
         // unused bank bits are not decoded: banks mirror like on real hardware
         uint32_t bank = data & 0x7F & bank_mask;
         uint32_t slot = bank >> 1;
         if (slot < BANKS_NUM) {
            rom_ptr = banks[slot] + ((bank & 0x01) ? 0x2000 : 0);
         }
         c64_set_exrom_game(0, 1);     // 8K
         rom_mask = ROM_MASK_8K;
      } else {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      }
   }

   // cartridge on/off takes effect during the write cycle (see early_commit
   // in c64_bus.h), the bank is switched at the end of the cycle
   CART_METHOD void early_commit(uint32_t control) {
      if (!(bus_addr(control) & 0xFF)) {
         if (bus_data(control) & 0x80)
            c64_set_exrom_game(1, 1);     // none
         else
            c64_set_exrom_game(0, 1);     // 8K
      }
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_magic_desk)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   MagicDeskCart cart = { p->crt_banks, p->crt_banks[0], p->bank_mask, ROM_MASK_8K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Ocean (5)
 *
 * $DE00 write: 8K bank (bits 0-5, unused bits not decoded), mapped at both
 * ROML and ROMH (16K mode) like VICE; 8K mode for small images (see CRTParser)
 * 8K banks are packed in crt_buf: bank n at crt_buf + n * 8K
 */
struct OceanCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *rom;
   uint8_t *rom_ptr;
   uint32_t bank_mask;
   uint32_t rom_mask;      // 8K or 16K, see CRTParser

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if (!(control & RW_MASK) && (bus_addr(control) == 0xDE00))
         rom_ptr = rom + ((uint32_t)(bus_data(control) & 0x3F & bank_mask) << 13);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_ocean)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   OceanCart cart = { p->crt_buf, p->crt_buf, p->bank_mask, p->rom_mask };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Fun Play, Power Play (7)
 *
 * $DE00 write: bank = bit 3-5 | bit 0 << 3, bit 7 = 1 cartridge disabled
 */
struct FunPlayCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t **banks;
   uint8_t *rom_ptr;
   uint32_t rom_mask;      // ROML when on, none when off

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      if ((control & RW_MASK) || (bus_addr(control) != 0xDE00))
         return;

      uint8_t data = bus_data(control);

      if (!(data & 0x80)) {
         c64_set_exrom_game(0, 1);     // 8K
         rom_ptr = banks[((data >> 3) & 0x07) | ((data & 0x01) << 3)];
         rom_mask = ROM_MASK_8K;
      } else {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      }
   }

   // cartridge on/off takes effect during the write cycle (see early_commit
   // in c64_bus.h), the bank is switched at the end of the cycle
   CART_METHOD void early_commit(uint32_t control) {
      if (bus_addr(control) == 0xDE00) {
         if (bus_data(control) & 0x80)
            c64_set_exrom_game(1, 1);     // none
         else
            c64_set_exrom_game(0, 1);     // 8K
      }
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_fun_play)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   FunPlayCart cart = { p->crt_banks, p->crt_banks[0], ROM_MASK_8K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Super Games (8)
 *
 * $DF00 write: bit 0-1 = 16K bank, bit 2 = 1 cartridge disabled,
 *              bit 3 = 1 register locked until reset
 */
struct SuperGamesCart {

   static constexpr uint32_t COMMIT_SEL = IO2_MASK;

   uint8_t **banks;
   uint8_t *rom_ptr;
   bool locked;
   uint32_t rom_mask;      // ROML+ROMH when on, none when off

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      if ((control & RW_MASK) || locked)
         return;

      uint8_t data = bus_data(control);

      rom_ptr = banks[data & 0x03];

      if (data & 0x04) {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      } else {
         c64_set_exrom_game(0, 0);     // 16K
         rom_mask = ROM_MASK_16K;
      }

      if (data & 0x08)
         locked = true;
   }

   // cartridge on/off takes effect during the write cycle (see early_commit
   // in c64_bus.h); bank and lock are applied at the end of the cycle
   CART_METHOD void early_commit(uint32_t control) {
      if (locked)
         return;
      if (bus_data(control) & 0x04)
         c64_set_exrom_game(1, 1);        // none
      else
         c64_set_exrom_game(0, 0);        // 16K
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_super_games)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   SuperGamesCart cart = { p->crt_banks, p->crt_banks[0], false, ROM_MASK_16K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * EasyFlash (32)
 *
 * $DE00 write: bank
 * $DE02 write: control register
 *   bit 0: GAME  (1 = /GAME asserted)
 *   bit 1: EXROM (1 = /EXROM asserted)
 *   bit 2: M     (1 = GAME driven by bit 0, 0 = GAME driven by boot jumper -> asserted)
 *   bit 7: LED   (ignored)
 * $DF00-$DFFF: 256 bytes RAM
 *
 * Mode table entry: bit 1 = EXROM line high, bit 0 = GAME line high
 * (same convention as c64_set_exrom_game(exrom, game))
 */
#define EF_MODE_16K        0x0      // EXROM low,  GAME low
#define EF_MODE_8K         0x1      // EXROM low,  GAME high
#define EF_MODE_ULTIMAX    0x2      // EXROM high, GAME low
#define EF_MODE_OFF        0x3      // EXROM high, GAME high

// keep it in SRAM, not in XIP flash (a cache miss would stall the bus loop)
static const uint8_t __not_in_flash("ef_mode") ef_mode_table[8] = {
   EF_MODE_ULTIMAX,  // 000: M=0 -> GAME from jumper (asserted), EXROM off
   EF_MODE_ULTIMAX,  // 001
   EF_MODE_16K,      // 010
   EF_MODE_16K,      // 011
   EF_MODE_OFF,      // 100
   EF_MODE_ULTIMAX,  // 101
   EF_MODE_8K,       // 110
   EF_MODE_16K       // 111
};

// ROM select lines answered in each mode (EF_MODE_*, see rom_mask)
static const uint32_t __not_in_flash("ef_mode") ef_rom_mask_table[4] = {
   ROM_MASK_16K,
   ROM_MASK_8K,
   ROM_MASK_ULTIMAX,
   ROM_MASK_OFF
};

struct EasyFlashCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK | IO2_MASK;

   uint8_t * const *roml_tab;
   uint8_t * const *romh_tab;
   uint8_t *roml_ptr;
   uint8_t *romh_ptr;
   bool ultimax;
   uint32_t rom_mask;      // ROM select lines enabled by the current mode
   uint8_t ram[256];

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         // same latency as a single 16K bank: select ROML/ROMH chunk without branching
         uint8_t *ptr = (control & ROML_MASK) ? romh_ptr : roml_ptr;
         data = ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      if (!(control & IO1_MASK)) {
         switch (bus_addr(control) & 0xFF) {
            case 0x09:     // $DE09 EF3 USB status
               data = ef3_rx_rdy | ef3_tx_rdy;
               return true;
            case 0x0A:     // $DE0A EF3 USB data (rx_rdy is cleared in commit)
               data = ef3_rx_data;
               return true;
         }
         return false;
      }
      if (!(control & IO2_MASK)) {
         data = ram[bus_addr(control) & 0xFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      uint32_t addr = bus_addr(control);

      if (control & RW_MASK) {
         // $DE0A read: byte consumed by the C64
         if (!(control & IO1_MASK) && (addr & 0xFF) == 0x0A)
            ef3_rx_rdy = EF3_NOT_RDY;
         return;
      }

      uint8_t data = bus_data(control);

      if (!(control & IO1_MASK)) {

         switch (addr & 0xFF) {

            case 0x00: {   // $DE00 bank register
               uint8_t bank = data & (EF_BANKS - 1);
               roml_ptr = roml_tab[bank];
               romh_ptr = romh_tab[bank];
               ef_cur_bank = bank;
               EF_TRACE(0x00, data);
               break;
            }

            case 0x0A:     // $DE0A EF3 USB data from C64
               ef3_tx_data = data;
               ef3_tx_rdy = EF3_NOT_RDY;
               break;

            case 0x02: {   // $DE02 control register
               uint8_t mode = ef_mode_table[data & 0x07];
               c64_set_exrom_game(mode >> 1, mode & 0x01);
               ultimax = (mode == EF_MODE_ULTIMAX);
               rom_mask = ef_rom_mask_table[mode];
               EF_TRACE(0x02, data);
               break;
            }
         }

      } else {
         // $DF00-$DFFF RAM
         ram[addr & 0xFF] = data;
      }
   }

   // $DE02 takes effect during the write cycle, at every sample (like the
   // original firmware): a cartridge disabled from ROM must already be off
   // at the next opcode fetch (e.g. Biggles)
   CART_METHOD void early_commit(uint32_t control) {
      if (!(control & IO1_MASK) && (bus_addr(control) & 0xFF) == 0x02) {
         uint8_t mode = ef_mode_table[bus_data(control) & 0x07];
         c64_set_exrom_game(mode >> 1, mode & 0x01);
      }
   }

   CART_METHOD bool vic_enabled(void) { return ultimax; }

   CART_METHOD uint8_t vic_read(uint32_t addr) {
      return romh_ptr[addr & 0x1FFF];
   }

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
   CART_METHOD void cpu_trace(uint32_t control) {
      cpu_trace_record(control);
   }
#endif

};

CART_FUNC void __time_critical_func(run_cart_easyflash)(void) {

   /* core1_args_t *p = */ multicore_fifo_pop_blocking();

   // EasyFlash boots in Ultimax mode (set by run_cart from CRT header), bank 0
   EasyFlashCart cart = { ef_roml, ef_romh, ef_roml[0], ef_romh[0], true,
                          ef_rom_mask_table[EF_MODE_ULTIMAX], {} };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Dinamic (17)
 *
 * any access to $DE00-$DE0F: bank = A0-A3
 */
struct DinamicCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t **banks;
   uint8_t *rom_ptr;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (!(control & ROML_MASK)) {
         data = rom_ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      rom_ptr = banks[bus_addr(control) & 0x0F];
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_dinamic)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   DinamicCart cart = { p->crt_banks, p->crt_banks[0] };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Zaxxon, Super Zaxxon (18)
 *
 * ROML: 4K ROM mirrored at $8000-$9FFF
 * ROMH: 8K bank selected by the last ROML read: $8000-$8FFF -> bank 0, $9000-$9FFF -> bank 1
 */
struct ZaxxonCart {

   static constexpr uint32_t COMMIT_SEL = ROML_MASK;

   uint8_t **banks;
   uint8_t *rom0_ptr;
   uint8_t *rom_ptr;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (!(control & ROML_MASK)) {
         data = rom0_ptr[bus_addr(control) & 0x0FFF];
         return true;
      }
      if (!(control & ROMH_MASK)) {
         data = rom_ptr[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if (control & RW_MASK)
         rom_ptr = banks[(bus_addr(control) & 0x1000) ? 1 : 0];
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_zaxxon)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   ZaxxonCart cart = { p->crt_banks, p->crt_banks[0], p->crt_banks[0] };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

void __time_critical_func(kff_set_command)(uint8_t cmd) {
   KFF_READ_PTR = 0;
   KFF_WRITE_PTR = 0;
   COMPILER_BARRIER();
   KFF_COMMAND = cmd;
}

bool __time_critical_func(kff_get_reply)(uint8_t cmd, uint8_t *reply) {
   *reply = KFF_COMMAND;
   if (cmd != *reply) {
       KFF_READ_PTR = 0;
       KFF_WRITE_PTR = 0;
       return true;
   }
   return false;
}

uint8_t __time_critical_func(kff_receive_byte)(void){
   return KFF_BUF[KFF_READ_PTR++];
}

void __time_critical_func(kff_send_byte)(uint8_t data) {
   KFF_BUF[KFF_WRITE_PTR++] = data;
}

void __time_critical_func(kff_init(void)) {
   c64_set_exrom_game(1, 0);     // Ultimax
   KFF_ID = KFF_ID_VALUE;
   kff_set_command(CMD_NONE);
}

//

void kff_dump(void) {
   uint16_t i = 0;

   printf("KFF_RAM\n");
   for(i=0; i<sizeof(kff_ram); i++)
      printf("%d) 0x%X [%c]\n", i, kff_ram[i], kff_ram[i]);
   printf("KFF_BUF\n");
   for(i=0; i<200; i++)
      printf("%d) 0x%X [%c]\n", i, KFF_BUF[i], KFF_BUF[i]);

   printf("KFF_READ_PTR: 0x%d\n", KFF_READ_PTR);
   printf("KFF_WRITE_PTR: 0x%d\n", KFF_WRITE_PTR);
}

/*
 * C64 "clean start": brings the C64 to a power-on like state before a
 * cartridge is started. The VIC-II has no reset line and keeps its registers
 * (graphics mode, sprites, raster IRQ), and a reset does not clear RAM: a
 * cartridge that boots directly could find the state left by the previous one.
 *
 * c64_clean_code is an Ultimax program at $E000 (ROMH) that:
 *  - sets VIC-II and SID registers to 0 (screen and sprites off)
 *  - copies a routine to $0200 and runs it with the cartridge off ($DE02)
 *    and all RAM visible ($01 = $34): clears RAM $0300-$FFFF, zero page and
 *    stack, then color RAM with I/O visible
 *  - writes $DE01 when done (about 0.7 s) and loops at $0240
 * Source and test: 6502 code assembled and checked with py65 on a C64 memory
 * model (RAM, color RAM, VIC-II and SID cleared).
 */
static const uint8_t c64_clean_code[] = {
   0x78, 0xA2, 0xFF, 0x9A, 0xD8, 0xA9, 0x2F, 0x85, 0x00, 0xA9, 0x37, 0x85,
   0x01, 0xA9, 0x00, 0x8D, 0x11, 0xD0, 0x8D, 0x15, 0xD0, 0x8D, 0x1A, 0xD0,
   0xA2, 0x2E, 0x9D, 0x00, 0xD0, 0xCA, 0x10, 0xFA, 0xA9, 0xFF, 0x8D, 0x19,
   0xD0, 0xA9, 0x00, 0xA2, 0x18, 0x9D, 0x00, 0xD4, 0xCA, 0x10, 0xFA, 0xA2,
   0x42, 0xBD, 0x3E, 0xE0, 0x9D, 0x00, 0x02, 0xCA, 0x10, 0xF7, 0x4C, 0x00,
   0x02, 0x40, 0xA9, 0x04, 0x8D, 0x02, 0xDE, 0xA9, 0x34, 0x85, 0x01, 0xA9,
   0x00, 0xA2, 0x03, 0x8E, 0x14, 0x02, 0xA0, 0x00, 0x99, 0x00, 0x03, 0xC8,
   0xD0, 0xFA, 0xE8, 0xD0, 0xF2, 0xA2, 0x02, 0x95, 0x00, 0xE8, 0xD0, 0xFB,
   0x9D, 0x00, 0x01, 0xE8, 0xD0, 0xFA, 0xA2, 0x35, 0x86, 0x01, 0xA2, 0x00,
   0x9D, 0x00, 0xD8, 0x9D, 0x00, 0xD9, 0x9D, 0x00, 0xDA, 0x9D, 0x00, 0xDB,
   0xE8, 0xD0, 0xF1, 0x8D, 0x01, 0xDE, 0x4C, 0x40, 0x02, 0xFF, 0xFF, 0xFF,
};

#define C64_CLEAN_RESET      0xE000      // reset vector
#define C64_CLEAN_IRQ        0xE03D      // NMI/IRQ vector: RTI
#define C64_CLEAN_TIMEOUT_US (3 * 1000 * 1000)

static volatile bool c64_clean_done;

struct CleanCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *romh;
   bool ultimax;
   uint32_t rom_mask;      // ROMH in Ultimax mode, none when off

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = romh[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   // the cartridge is turned off during the write cycle (see c64_bus.h)
   CART_METHOD void early_commit(uint32_t control) {
      if ((bus_addr(control) & 0xFF) == 0x02)
         c64_set_exrom_game(1, 1);
   }

   CART_METHOD void commit(uint32_t control) {
      if (control & RW_MASK)
         return;
      switch (bus_addr(control) & 0xFF) {
         case 0x01:     // $DE01: clean done
            c64_clean_done = true;
            break;
         case 0x02:     // $DE02: cartridge off
            c64_set_exrom_game(1, 1);
            ultimax = false;
            rom_mask = ROM_MASK_OFF;
            break;
      }
   }

   CART_METHOD bool vic_enabled(void) { return ultimax; }

   CART_METHOD uint8_t vic_read(uint32_t addr) {
      return romh[addr & 0x1FFF];
   }
};

CART_FUNC void __time_critical_func(run_cart_clean)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   CleanCart cart = { p->crt_buf, true, ROMH_MASK };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

bool run_c64_clean(void) {

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   cart_stop();

   // 8K ROMH image in crt_buf (any cartridge started after this reloads it)
   uint8_t *romh = crt_buf;
   memset(romh, 0xFF, 8*1024);
   memcpy(romh, c64_clean_code, sizeof(c64_clean_code));
   romh[0x1FFA] = C64_CLEAN_IRQ & 0xFF;      // NMI
   romh[0x1FFB] = C64_CLEAN_IRQ >> 8;
   romh[0x1FFC] = C64_CLEAN_RESET & 0xFF;    // RESET
   romh[0x1FFD] = C64_CLEAN_RESET >> 8;
   romh[0x1FFE] = C64_CLEAN_IRQ & 0xFF;      // IRQ
   romh[0x1FFF] = C64_CLEAN_IRQ >> 8;

   c64_clean_done = false;
   c64_set_exrom_game(1, 0);     // Ultimax
   args = {};
   args.crt_buf = romh;
   multicore_launch_core1(run_cart_clean);
   multicore_fifo_push_blocking((uint32_t)&args);

   uint64_t t0 = time_us_64();
   while (!c64_clean_done && time_us_64() - t0 < C64_CLEAN_TIMEOUT_US)
      tight_loop_contents();

   bool done = c64_clean_done;
   if (!done)
      printf("E: C64 clean start timeout\n");

   // keep the C64 in reset until the next cartridge is started
   cart_stop();
   return done;
}

/*
 * KungFuFlash launcher
 *
 * $DE00 read/write: data register (KFF_BUF stream)
 * $DE02 write: bit 0 = 1 16K, 0 none
 * $DE01-$DEFF: KFF RAM (command, pointers, ID)
 *
 * KFF RAM and buffer are shared with core0: always accessed as volatile
 */
#define KFF_READ_PTR_V     (*(volatile uint16_t *)(kff_ram + 4))
#define KFF_WRITE_PTR_V    (*(volatile uint16_t *)(kff_ram + 6))
#define KFF_BUF_V          ((volatile uint8_t *)KFF_BUF)

struct KffCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *rom;
   bool ultimax;
   uint32_t rom_mask;      // Ultimax at boot, then 16K or none ($DE02)

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {

      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }

      if (!(control & IO1_MASK)) {
         uint8_t reg = bus_addr(control) & 0xFF;
         // no pointer increment here: it is done once per cycle in commit()
         data = (reg == 0x00) ? KFF_BUF_V[KFF_READ_PTR_V] : kff_ram[reg];
         return true;
      }

      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      uint8_t reg = bus_addr(control) & 0xFF;

      if (control & RW_MASK) {
         // $DE00 read: advance read pointer
         if (reg == 0x00)
            KFF_READ_PTR_V = KFF_READ_PTR_V + 1;
         return;
      }

      uint8_t data = bus_data(control);

      switch (reg) {

         case 0x00:     // $DE00 data register
            KFF_BUF_V[KFF_WRITE_PTR_V] = data;
            KFF_WRITE_PTR_V = KFF_WRITE_PTR_V + 1;
            break;

         case 0x02:     // $DE02 control register
            if (data & 0x01) {
               c64_set_exrom_game(0, 0);     // 16K ROM
               rom_mask = ROM_MASK_16K;
            } else {
               c64_set_exrom_game(1, 1);     // none
               rom_mask = ROM_MASK_OFF;
            }
            ultimax = false;
            break;

         default:
            kff_ram[reg] = data;
            break;
      }
   }

   // $DE02 takes effect during the write cycle (see early_commit in c64_bus.h)
   CART_METHOD void early_commit(uint32_t control) {
      if ((bus_addr(control) & 0xFF) == 0x02) {
         if (bus_data(control) & 0x01)
            c64_set_exrom_game(0, 0);     // 16K ROM
         else
            c64_set_exrom_game(1, 1);     // none
      }
   }

   CART_METHOD bool vic_enabled(void) { return ultimax; }

   CART_METHOD uint8_t vic_read(uint32_t addr) {
      return rom[(addr & 0x1FFF) | 0x2000];
   }
};

CART_FUNC void __time_critical_func(run_cart_kff)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();

   // launcher boots in Ultimax mode (see kff_init)
   KffCart cart = { p->crt_buf, true, ROM_MASK_ULTIMAX };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * EF3 USB mode: used to load PRG files
 *
 * The launcher ROM is started again in EasyFlash 3 mode ($DE09 does not
 * return KFF_ID_VALUE): it gets the PRG from core0 through the EF3 USB
 * registers, then runs it from BASIC. The loader code lives in the
 * EasyFlash RAM at $DF00, which stays visible after the cartridge is disabled.
 *
 * $DE00 write: bank (launcher has a single 16K bank, ignored)
 * $DE02 write: EasyFlash control register
 * $DE09 read:  USB status (bit 7 = RX ready, bit 6 = TX ready)
 * $DE0A read:  USB data from core0 / write: USB data to core0
 * $DF00-$DFFF: 256 bytes RAM
 */
// offset of the "LOADING" text in EasyFlash RAM (see launcher/commands.h)
#ifndef LOADING_OFFSET
#define LOADING_OFFSET  0x80
#endif


struct Ef3Cart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK | IO2_MASK;

   uint8_t *rom;
   bool ultimax;
   uint32_t rom_mask;      // ROM select lines of the EasyFlash mode

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {

      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }

      if (!(control & IO1_MASK)) {
         switch (bus_addr(control) & 0xFF) {
            case 0x09:     // $DE09 USB status
               data = ef3_rx_rdy | ef3_tx_rdy;
               return true;
            case 0x0A:     // $DE0A USB data (rx_rdy is cleared in commit)
               data = ef3_rx_data;
               return true;
         }
         return false;
      }

      if (!(control & IO2_MASK)) {
         data = ef3_ram[bus_addr(control) & 0xFF];
         return true;
      }

      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      uint8_t reg = bus_addr(control) & 0xFF;

      if (control & RW_MASK) {
         // $DE0A read: byte consumed by the C64
         if (!(control & IO1_MASK) && reg == 0x0A)
            ef3_rx_rdy = EF3_NOT_RDY;
         return;
      }

      uint8_t data = bus_data(control);

      if (!(control & IO1_MASK)) {

         switch (reg) {

            case 0x02: {   // $DE02 control register
               uint8_t mode = ef_mode_table[data & 0x07];
               c64_set_exrom_game(mode >> 1, mode & 0x01);
               ultimax = (mode == EF_MODE_ULTIMAX);
               rom_mask = ef_rom_mask_table[mode];
               break;
            }

            case 0x0A:     // $DE0A USB data from C64
               ef3_tx_data = data;
               ef3_tx_rdy = EF3_NOT_RDY;
               break;
         }

      } else {
         // $DF00-$DFFF RAM
         ef3_ram[reg] = data;
      }
   }

   // $DE02 takes effect during the write cycle (see EasyFlashCart)
   CART_METHOD void early_commit(uint32_t control) {
      if ((bus_addr(control) & 0xFF) == 0x02 && !(control & IO1_MASK)) {
         uint8_t mode = ef_mode_table[bus_data(control) & 0x07];
         c64_set_exrom_game(mode >> 1, mode & 0x01);
      }
   }

   CART_METHOD bool vic_enabled(void) { return ultimax; }

   CART_METHOD uint8_t vic_read(uint32_t addr) {
      return rom[(addr & 0x1FFF) | 0x2000];
   }
};

CART_FUNC void __time_critical_func(run_cart_ef3)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();

   // launcher boots in Ultimax mode
   Ef3Cart cart = { p->crt_buf, true, ROM_MASK_ULTIMAX };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

// core0 side of the EF3 USB protocol

#define EF3_TIMEOUT_US   (5 * 1000 * 1000)

static bool ef3_putc(uint8_t data) {

   uint64_t t0 = time_us_64();
   while (ef3_rx_rdy) {
      if (time_us_64() - t0 > EF3_TIMEOUT_US)
         return false;
   }

   ef3_rx_data = data;
   __dmb();
   ef3_rx_rdy = EF3_RX_RDY;
   return true;
}

static bool ef3_getc(uint8_t *data) {

   uint64_t t0 = time_us_64();
   while (ef3_tx_rdy) {
      if (time_us_64() - t0 > EF3_TIMEOUT_US)
         return false;
   }

   *data = ef3_tx_data;
   __dmb();
   ef3_tx_rdy = EF3_TX_RDY;
   return true;
}

static bool ef3_send_data(const void *buffer, size_t size) {
   const uint8_t *ptr = (const uint8_t *)buffer;
   while (size--) {
      if (!ef3_putc(*ptr++))
         return false;
   }
   return true;
}

static bool ef3_receive_data(void *buffer, size_t size) {
   uint8_t *ptr = (uint8_t *)buffer;
   while (size--) {
      if (!ef3_getc(ptr++))
         return false;
   }
   return true;
}

/*
 * Load and run a PRG file (2 byte load address + data) through the launcher
 * in EF3 USB mode. The C64 is reset. Returns false if the transfer failed.
 */
bool run_prg(const uint8_t *prg, uint32_t size, const char *name) {

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   if (size <= 2 || size >= PRG_BUF_SIZE) {
      printf("E: PRG size %lu not valid\n", (unsigned long)size);
      return false;
   }

   c64_clean_before_start();
   cart_stop();

   // launcher ROM copy (crt_buf is not cleared: prg may be inside it)
   memcpy(crt_buf, CRT_LAUNCHER, CRT_LAUNCHER_SIZE);

   // "LOADING <name>" printed by the loader at BASIC start-up ($DF80)
   memset(ef3_ram, 0, sizeof(ef3_ram));
   char *dest = convert_to_screen_code((char *)ef3_ram + LOADING_OFFSET, "LOADING ");
   char text[33];
   strncpy(text, name, sizeof(text) - 1);
   text[sizeof(text) - 1] = 0;
   dest = convert_to_screen_code(dest, text);
   *dest = 0;

   printf("\nprg: %s (%lu bytes)\n", name, (unsigned long)size);

   // exit from launcher
   launcher_disable();

   c64_set_exrom_game(1, 0);     // Ultimax
   args = {};
   args.crt_buf = crt_buf;
   multicore_launch_core1(run_cart_ef3);
   multicore_fifo_push_blocking((uint32_t)&args);

   // command: "EFSTART:PRG", expected reply: "LOAD"
   char reply[5];
   if (!ef3_send_data("EFSTART:PRG", 12) || !ef3_receive_data(reply, 5) ||
       memcmp(reply, "LOAD", 5) != 0) {
      printf("E: EF3 PRG command failed\n");
      return false;
   }

   // C64 requests a size, we send the actual size and the data
   uint16_t req_size, tx_size;
   if (!ef3_receive_data(&req_size, 2))
      return false;

   tx_size = (size > req_size) ? req_size : size;
   if (!ef3_send_data(&tx_size, 2) || !ef3_send_data(prg, tx_size)) {
      printf("E: EF3 PRG transfer failed\n");
      return false;
   }

   // close: two zero bytes
   uint8_t close[2];
   if (!ef3_receive_data(close, 2) || close[0] || close[1]) {
      printf("E: EF3 PRG close failed\n");
      return false;
   }

   printf("done\n");
   return tx_size == size;
}

//

//

/*
 * Kung Fu Flash EAPI service (core0)
 *
 * The EAPI driver sends "KFF:" + command through the EF3 USB data register:
 *   CMD_EAPI_INIT                          -> reply 0
 *   CMD_WRITE_FLASH  addr_lo addr_hi value -> reply 0 / REPLY_WRITE_ERROR
 *   CMD_ERASE_SECTOR addr_lo addr_hi bank  -> reply 0 / REPLY_WRITE_ERROR
 *   CMD_STREAM_TEST                        -> endless stream 0, 1, 2... 255, 0...
 *                                             until the C64 stops reading (test
 *                                             of the $DE0A reads, see
 *                                             utils/mkcart_streamtest.py)
 * The bank is the one selected in $DE00 before the command.
 * Changes are kept in RAM only (lost on reset/power off).
 */

// EAPI protocol (see launcher/eapi-kung_fu_flash.s)
#define CMD_EAPI_INIT       0x01
#define CMD_WRITE_FLASH     0x02
#define CMD_ERASE_SECTOR    0x03
#define CMD_STREAM_TEST     0x10
#define REPLY_EAPI_OK       0x00
#define REPLY_WRITE_ERROR   0x02

// get the chunk of an EasyFlash bank half, allocating it if it is the
// shared erased chunk (copy on write); returns NULL if no memory is left
static uint8_t *eapi_get_chunk(uint8_t bank, bool romh) {

   uint8_t **tab = romh ? ef_romh : ef_roml;
   uint8_t *chunk = tab[bank];

   if (chunk == ef_blank) {
      if ((ef_next_chunk + 1) * EF_CHUNK_SIZE > EF_BUF_LIMIT)
         return NULL;
      chunk = crt_buf + ef_next_chunk * EF_CHUNK_SIZE;
      ef_next_chunk++;
      memset(chunk, 0xFF, EF_CHUNK_SIZE);
      // core1 picks it up at the next $DE00 write (EAPI always sets the bank)
      tab[bank] = chunk;
   }

   return chunk;
}

uint8_t *ef_chunk(uint8_t bank, bool romh, bool alloc) {
   if (alloc)
      return eapi_get_chunk(bank, romh);
   return romh ? ef_romh[bank] : ef_roml[bank];
}

#ifdef DEBUG
static uint8_t eapi_dbg_old;      // flash value before the last write
#endif

static uint8_t eapi_write_flash(uint16_t addr, uint8_t value) {

   // $8000-$9FFF ROML, $A000-$BFFF / $E000-$FFFF ROMH
   uint8_t *chunk = eapi_get_chunk(ef_cur_bank, addr & 0x2000);
   if (!chunk)
      return REPLY_WRITE_ERROR;

   uint8_t *dest = chunk + (addr & 0x1FFF);
#ifdef DEBUG
   eapi_dbg_old = *dest;
#endif
   *dest &= value;      // flash can only clear bits
   ef_save_mark(ef_cur_bank, addr & 0x2000);

   return (*dest == value) ? REPLY_EAPI_OK : REPLY_WRITE_ERROR;
}

static uint8_t eapi_erase_sector(uint16_t addr, uint8_t bank) {

   // a 64K sector covers 8 banks of ROML or ROMH
   if (bank >= EF_BANKS || (bank % 8))
      return REPLY_WRITE_ERROR;

   bool romh = (addr & 0xFF00) != 0x8000;
   uint8_t **tab = romh ? ef_romh : ef_roml;

   for (uint8_t b = bank; b < bank + 8; b++) {
      if (tab[b] != ef_blank) {
         memset(tab[b], 0xFF, EF_CHUNK_SIZE);
         ef_save_mark(b, romh);
      }
   }

   return REPLY_EAPI_OK;
}

#if defined(DEBUG) && EF_DEBUG_TRACE
// print EasyFlash register writes, repeated $DE00 writes are counted
static void ef_trace_print(void) {

   static uint32_t tail;
   static uint16_t last = 0xFFFF;
   static uint32_t repeat;

   uint32_t head = ef_trace_head;
   if (head - tail > EF_TRACE_SIZE) {
      printf("EF trace: %lu entries lost\n", (unsigned long)(head - tail - EF_TRACE_SIZE));
      tail = head - EF_TRACE_SIZE;
   }

   while (tail != head) {
      uint16_t e = ef_trace[tail & (EF_TRACE_SIZE - 1)];
      tail++;
      if (e == last) {
         repeat++;
         continue;
      }
      if (repeat)
         printf("   (x%lu)\n", (unsigned long)repeat);
      repeat = 0;
      last = e;
      printf("EF $DE%02X=$%02X\n", e >> 8, e & 0xFF);
   }
}
#endif


#ifdef DEBUG
// number of EAPI flash writes not printed yet
static uint32_t eapi_dbg_writes;
static uint32_t eapi_dbg_failed;
// first and last write of the sequence (bank << 16 | address)
static uint32_t eapi_dbg_first, eapi_dbg_last;

static void eapi_dbg_report(void) {
   if (eapi_dbg_writes)
      printf("EAPI: %lu bytes written (%lu failed), bank %lu $%04lX .. bank %lu $%04lX\n",
             (unsigned long)eapi_dbg_writes, (unsigned long)eapi_dbg_failed,
             (unsigned long)(eapi_dbg_first >> 16), (unsigned long)(eapi_dbg_first & 0xFFFF),
             (unsigned long)(eapi_dbg_last >> 16), (unsigned long)(eapi_dbg_last & 0xFFFF));
   eapi_dbg_writes = 0;
   eapi_dbg_failed = 0;
}
#endif

void cart_poll(void) {

   // write the EasyFlash save file when the cartridge stopped writing
   if (ef_save_due()) {
#ifdef DEBUG
      eapi_dbg_report();
#endif
      ef_save_flush();
   }

#if defined(DEBUG) && EF_DEBUG_TRACE
   if (eapi_active)
      ef_trace_print();
#endif

   if (!eapi_active || ef3_tx_rdy)
      return;     // no data from the C64

   uint8_t c;
   if (!ef3_getc(&c))
      return;

   memmove(eapi_window, eapi_window + 1, sizeof(eapi_window) - 1);
   eapi_window[sizeof(eapi_window) - 1] = c;

   if (memcmp(eapi_window, "KFF:", 4) != 0)
      return;

   uint8_t cmd = eapi_window[4];
   memset(eapi_window, 0, sizeof(eapi_window));

   if (cmd == CMD_STREAM_TEST) {
      // ends when the C64 stops reading (ef3_putc timeout)
      uint8_t value = 0;
      uint32_t count = 0;
#ifdef DEBUG
      printf("EF3 stream test: started\n");
#endif
      while (ef3_putc(value)) {
         value++;
         count++;
      }
#ifdef DEBUG
      printf("EF3 stream test: %lu bytes sent\n", (unsigned long)count);
#endif
      return;
   }

   uint8_t reply = REPLY_EAPI_OK;
   uint16_t addr = 0;
   uint8_t wr_value = 0;     // value (write) or bank (erase)

   if (cmd == CMD_WRITE_FLASH || cmd == CMD_ERASE_SECTOR) {
      uint8_t args[3];    // addr_lo, addr_hi, value/bank
      if (!ef3_receive_data(args, sizeof(args)))
         return;

      addr = args[0] | (args[1] << 8);
      wr_value = args[2];
      reply = (cmd == CMD_WRITE_FLASH) ? eapi_write_flash(addr, wr_value)
                                       : eapi_erase_sector(addr, wr_value);
   }

   // reply first: the EAPI init waits for it only a few ms
   ef3_putc(reply);

#ifdef DEBUG
   // writes are only counted: printing every byte would slow down the EAPI
   if (cmd == CMD_WRITE_FLASH) {
      uint32_t pos = ((uint32_t)ef_cur_bank << 16) | addr;
      if (!eapi_dbg_writes)
         eapi_dbg_first = pos;
      eapi_dbg_last = pos;
      eapi_dbg_writes++;
      if (reply != REPLY_EAPI_OK && ++eapi_dbg_failed <= 8)
         printf("EAPI: write bank %u $%04X: $%02X -> $%02X failed\n",
                ef_cur_bank, addr, eapi_dbg_old, wr_value);
   } else {
      eapi_dbg_report();
      if (cmd == CMD_ERASE_SECTOR)
         printf("EAPI: erase bank %u $%04X -> %u\n", wr_value, addr, reply);
      else
         printf("EAPI: command %u\n", cmd);
   }
#endif
}

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
 *   cputrace stop         stop recording now
 *   cputrace run          clear and record again
 * Line format: cycle (relative to the trigger or to the last one), address,
 * R/W, data, then the asserted lines (ROML ROMH IO1 IO2) and "ba" when the
 * VIC-II requests the bus (BA low).
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
      printf("%+6ld %04lX %c %02X%s%s%s%s%s\n", (long)(i - ref),
             (unsigned long)bus_addr(c), (c & RW_MASK) ? 'r' : 'W', bus_data(c),
             (c & ROML_MASK) ? "" : " ROML", (c & ROMH_MASK) ? "" : " ROMH",
             (c & IO1_MASK) ? "" : " IO1", (c & IO2_MASK) ? "" : " IO2",
             (c & BA_MASK) ? "" : " ba");
   }
}
#else
void cpu_trace_shell(char *args) {
   (void)args;
   printf("cputrace: not enabled (EF_DEBUG_CPU_TRACE in cartridge.cpp)\n");
}
#endif
