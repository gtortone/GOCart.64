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
#include "config.h"
#include "psram.h"
#include "cartridge_int.h"
#include "ef_cache.h"
#include "bus_diag.h"
#include <string.h>
#include <stdlib.h>     // strtoul

// aligned: 8K chunks are copied with 32-bit accesses (EasyFlash PSRAM cache)
uint8_t crt_buf[CRT_BUFFER_SIZE] __attribute__((aligned(8))) = {};
uint8_t crt_map[EF_MAP_SIZE] = {};


// EasyFlash bank register, published by core1 for EAPI commands on core0
volatile uint8_t ef_cur_bank;

// EAPI commands are served by cart_poll() on core0
bool eapi_active = false;

// GMod2 EEPROM (see the GMod2 handler)
#define GMOD2_EEP_WORDS   1024
#define GMOD2_EEP_SIZE    (GMOD2_EEP_WORDS * 2)

static uint8_t *gmod2_eep;                    // NULL: no room, EEPROM absent
static volatile uint32_t gmod2_eep_changes;   // EEPROM writes (core0 saves the file)
static volatile uint32_t gmod2_eep_fill_req;  // ERAL/WRAL for core0: GMOD2_FILL_REQ | word
static bool gmod2_active;                     // GMod2 running (cart_poll)

#define GMOD2_FILL_REQ    0x10000

// core0: ERAL / WRAL requested by core1, 2K at once
static void gmod2_eep_fill_pending(void) {
   uint32_t req = gmod2_eep_fill_req;
   if (!req || !gmod2_eep)
      return;
   for (uint32_t i = 0; i < GMOD2_EEP_WORDS; i++) {
      gmod2_eep[2 * i] = (uint8_t)(req >> 8);
      gmod2_eep[2 * i + 1] = (uint8_t)req;
   }
   __dmb();
   gmod2_eep_fill_req = 0;   // ready
}



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
 * EF3 USB registers ($DE09 status, $DE0A data), used by:
 *  - the launcher in EF3 mode, to load PRG files
 *  - the Kung Fu Flash EAPI installed in EasyFlash cartridges
 */
// EF3_RX_RDY, EF3_TX_RDY, EF3_NOT_RDY: see cartridge_int.h

// shared between core0 (ef3_putc/ef3_getc) and core1 (bus loop)
volatile uint8_t ef3_rx_rdy = EF3_NOT_RDY;   // data for C64 available
volatile uint8_t ef3_tx_rdy = EF3_TX_RDY;    // C64 may send data
volatile uint8_t ef3_rx_data;
volatile uint8_t ef3_tx_data;
static uint8_t ef3_ram[256];

// last bytes received from the C64, to recognize "KFF:" EAPI commands (cart_poll)
static uint8_t eapi_window[5];

void eapi_start(void) {
   memset(eapi_window, 0, sizeof(eapi_window));
   eapi_active = true;
}

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
   c64_dma_bus_restore();  // core1 may have been stopped during a DMA pause
   c64_dma_release();

   // write the GMod2 EEPROM file, if changed, before crt_buf is reused
   if (gmod2_active) {
      gmod2_eep_fill_pending();
      gmod2_eep_stop(gmod2_eep_changes);
   }
   gmod2_eep_fill_req = 0;
   gmod2_active = false;
   gmod2_eep = NULL;
   ef_cache_stop();

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
   CRT_BANK(27)
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



uint8_t run_cart(IDataReader &r, const char *file_name) {

   c64_clean_before_start();
   cart_stop();

   // init cart memory
   memset(crt_buf, 0, sizeof(crt_buf));
   memset(crt_map, 0, sizeof(crt_map));

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   // a CRT larger than crt_buf is loaded into the PSRAM (EasyFlash, Ocean,
   // C64GS, Magic Desk, see the PSRAM chunk cache), crt_buf is the transfer
   // buffer
   bool to_psram = psram_size() >= EF_PSRAM_MAX && r.size() > sizeof(crt_buf);
   PsramBounceReader pr(r, crt_buf, sizeof(crt_buf));
   CRTParser *crt = to_psram ?
      new CRTParser(pr, PSRAM_UNCACHED(0), EF_PSRAM_MAX, crt_map, sizeof(crt_map)) :
      new CRTParser(r, crt_buf, sizeof(crt_buf), crt_map, sizeof(crt_map));
   if (to_psram)
      printf("cart: %lu bytes, loading into the PSRAM\n", (unsigned long)r.size());
   bool parsed = crt->parse();
   // PSRAM: data checked after the load, loaded again if it differs
   for (int retry = 0; to_psram && parsed; retry++) {
      bool ok = pr.check();
      printf("PSRAM check: %lu bytes %s\n", (unsigned long)pr.bytes(), ok ? "OK" : "FAILED");
      if (ok)
         break;
      if (retry == 2) {
         parsed = false;
         break;
      }
      delete crt;
      pr.restart();
      crt = new CRTParser(pr, PSRAM_UNCACHED(0), EF_PSRAM_MAX, crt_map, sizeof(crt_map));
      parsed = pr.seek(0) && crt->parse();
   }
   uint16_t type = parsed ? crt->getType() : 0;
   bool psram_type = type == 32 || type == 5 || type == 15 || type == 19 || type == 60;
   if (parsed != true || (to_psram && !psram_type)) {
      if (to_psram && parsed && !psram_type)
         printf("E: cartridge too large (only EasyFlash, Ocean, C64GS, Magic Desk, GMod2 can use the PSRAM)\n");
      printf("E: CRT parsing error\n");
      delete crt;
      c64_release_reset();      // no cartridge: the C64 starts BASIC
      return 1;
   }
   ef_psram = to_psram;

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
         packed_build_tables(crt->getEasyFlashChunks(), "Ocean");
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
         packed_build_tables(crt->getEasyFlashChunks(), "Magic Desk");
         printf("cart: Magic Desk\n");
         multicore_launch_core1(run_cart_magic_desk);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 4:
         // Simons' BASIC (4)
         args.crt_buf = crt_buf;
         printf("cart: Simons' BASIC\n");
         multicore_launch_core1(run_cart_simons_basic);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 11:
         // Westermann Learning (11)
         args.crt_buf = crt_buf;
         printf("cart: Westermann Learning\n");
         multicore_launch_core1(run_cart_westermann);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 12:
         // Rex Utility (12)
         args.crt_buf = crt_buf;
         printf("cart: Rex Utility\n");
         multicore_launch_core1(run_cart_rex_utility);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 13:
         // Final Cartridge I (13)
         args.crt_buf = crt_buf;
         printf("cart: Final Cartridge I\n");
         multicore_launch_core1(run_cart_final_i);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 15:
         // C64 Game System, System 3 (15)
         packed_build_tables(crt->getEasyFlashChunks(), "C64GS");
         printf("cart: C64 Game System\n");
         multicore_launch_core1(run_cart_c64gs);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 16:
         // Warp Speed (16)
         args.crt_buf = crt_buf;
         printf("cart: Warp Speed\n");
         multicore_launch_core1(run_cart_warp_speed);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 21:
         // Comal-80 (21)
         args.crt_banks = crt_banks;
         printf("cart: Comal-80\n");
         multicore_launch_core1(run_cart_comal80);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 43:
         // Prophet64 (43)
         args.crt_buf = crt_buf;
         printf("cart: Prophet64\n");
         multicore_launch_core1(run_cart_prophet64);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 60: {
         // GMod2 (60): in the PSRAM the last cache slot holds the EEPROM
         uint8_t chunks = crt->getEasyFlashChunks();
         if (ef_psram)
            ef_nslots = EF_SLOTS - 1;
         packed_build_tables(chunks, "GMod2");
         if (ef_psram)
            gmod2_eep = EF_SLOT(EF_SLOTS - 1);
         else if ((uint32_t)(chunks + 2) * EF_CHUNK_SIZE <= sizeof(crt_buf))
            gmod2_eep = crt_buf + (uint32_t)(chunks + 1) * EF_CHUNK_SIZE;
         else
            printf("W: GMod2: no room for the EEPROM\n");
         if (gmod2_eep)
            gmod2_eep_start(file_name, gmod2_eep, GMOD2_EEP_SIZE);
         gmod2_eep_changes = 0;
         gmod2_eep_fill_req = 0;
         gmod2_active = true;
         printf("cart: GMod2\n");
         multicore_launch_core1(run_cart_gmod2);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;
      }

      case 57:
         // RGCD, Hucky (57)
         args.crt_buf = crt_buf;
         args.subtype = crt->getSubtype();
         printf("cart: %s\n", args.subtype == 1 ? "Hucky" : "RGCD");
         multicore_launch_core1(run_cart_rgcd);
         multicore_fifo_push_blocking((uint32_t)&args);
         break;

      case 32:
         // EasyFlash (32)
         easyflash_build_tables(crt->getEasyFlashChunks());
         if (!ef_psram && strcmp(crt->getName(), BUS_CAL_CRT_NAME) == 0) {
            // bus timing calibration (utils/mkcart_buscal.py)
            args.crt_banks = crt_banks;
            args.crt_map = crt_map;
            bus_calibrate(&args);
            break;
         }
#ifdef DEBUG
         if (bus_scan_pending()) {
            // edge delay scan with this cartridge (shell "bus scan")
            if (ef_psram)
               ef_cache_fill(crt->getEasyFlashChunks());
            args.crt_banks = crt_banks;
            args.crt_map = crt_map;
            bus_scan_ef(&args, crt->getExrom(), crt->getGame());
            break;
         }
#endif
         ef_save_start(file_name, crt->getSize(), ef_crt_hash);   // apply "<name>.sav", if any
         if (ef_psram)
            ef_cache_fill(crt->getEasyFlashChunks());
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
 * Handler code runs on core1 from RAM (__time_critical_func): a flash (XIP)
 * cache miss would stall the bus loop. Main RAM is almost full (DEBUG build),
 * so some handlers are placed in the 4K scratch banks, which only hold the
 * core stacks (2K each): SCRATCH_X (core1 stack) and SCRATCH_Y (core0 stack).
 */
#define CART_IN_SCRATCH_X(name)   __scratch_x(#name) name
#define CART_IN_SCRATCH_Y(name)   __scratch_y(#name) name

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

// ROM select lines of a cartridge mode (same arguments as c64_set_exrom_game),
// for the core1 handlers (inline: no flash access)
static inline __attribute__((always_inline)) uint32_t rom_mask_of(bool exrom, bool game) {
   return exrom ? (game ? ROM_MASK_OFF : ROM_MASK_ULTIMAX) : (game ? ROM_MASK_8K : ROM_MASK_16K);
}

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
 * 8K banks packed as chunks: ef_tab[bank] (see parse8KPackedChip), in the
 * PSRAM for images larger than crt_buf (up to 1 MB)
 */
struct MagicDeskCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

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
         rom_ptr = ef_bank_chunk(data & 0x7F & bank_mask);
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
   MagicDeskCart cart = { ef_tab[0], p->bank_mask, ROM_MASK_8K };

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
         rom_ptr = ef_bank_chunk(bus_data(control) & 0x3F & bank_mask);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_ocean)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   OceanCart cart = { ef_tab[0], p->bank_mask, p->rom_mask };

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
   // no per-cart EDGE_DELAY: the bus calibration runs this handler, so the
   // edge delay it finds must be the global one used by every cartridge

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
               uint8_t *l = roml_tab[bank];
               uint8_t *h = romh_tab[bank];
               if (__builtin_expect(!l || !h, 0)) {
                  // PSRAM cache miss: the 6510 waits (DMA)
                  ef_cache_miss(bank);
                  l = roml_tab[bank];
                  h = romh_tab[bank];
               }
               roml_ptr = l;
               romh_ptr = h;
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

#ifdef DEBUG
// EasyFlash with crash detection, for the edge delay scan (shell "bus scan",
// see bus_diag.cpp)
struct EasyFlashScanCart : EasyFlashCart {

   BusCrashDetect crash;

   // once per cycle, with the last CPU phase sample (c64_bus_loop)
   CART_METHOD void cpu_trace(uint32_t control) {
      crash.cycle(control);
   }
};

CART_FUNC void __time_critical_func(run_cart_easyflash_scan)(void) {

   multicore_fifo_pop_blocking();

   EasyFlashScanCart cart = { { ef_roml, ef_romh, ef_roml[0], ef_romh[0], true,
                                ef_rom_mask_table[EF_MODE_ULTIMAX], {} }, {} };

   c64_reset();
   save_and_disable_interrupts();

   c64_bus_loop(cart);
}
#endif

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

/*
 * C64 Game System, System 3 (15)
 *
 * up to 64 banks of 8K at ROML (8K mode), packed as chunks: ef_tab[bank]
 * (see CRTParser::parse8KPackedChip), in the PSRAM for images larger than
 * crt_buf
 * any access (read or write) to $DE00-$DEFF: bank = A0-A5
 */
struct C64GsCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *rom_ptr;
   uint32_t bank_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, ROM_MASK_8K)) {
         data = rom_ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      rom_ptr = ef_bank_chunk(bus_addr(control) & 0x3F & bank_mask);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_X(run_cart_c64gs)(void) {

   /* core1_args_t *p = */ multicore_fifo_pop_blocking();
   C64GsCart cart = { ef_tab[0], EF_BANKS - 1 };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * GMod2 (60), Individual Computers
 *
 * 512K flash: up to 64 banks of 8K at ROML, packed as chunks: ef_tab[bank]
 * (see parse8KPackedChip), in the PSRAM for images larger than crt_buf.
 * $DE00-$DEFF write, bits 7-6 = mode:
 *   00  8K mode, bits 5-0 = bank
 *   01  ROM off (EXROM high), 93C86 EEPROM selected: bit 5 clock, bit 4 data in
 *   11  flash write mode: not emulated (8K mode, the bank as in 00, flash
 *       writes ignored)
 * $DE00-$DEFF read while the EEPROM is selected: bit 7 = EEPROM data out.
 * Power on / reset: 8K mode, bank 0, EEPROM not selected.
 *
 * EEPROM (gmod2_eep, 2K, see gmod2_eep_start): M93C86 in 16 bit mode, 1024
 * words, Microwire: start bit, 2 bit opcode, 10 bit address, sampled on the
 * clock rising edge. READ: a dummy 0, then 16 bits per word MSB first,
 * sequential. WRITE/ERASE/ERAL/WRAL need EWEN, they are done when chip select
 * goes low (at once: ready, data out 1, as soon as it is selected again).
 * ERAL/WRAL (2K at once) are done by core0 (gmod2_eep_fill_pending): data
 * out reads 0 (busy) until then, as on the real chip.
 */

enum { EEP_IDLE, EEP_CMD, EEP_DATA, EEP_READ, EEP_DONE };
enum { EEP_OP_NONE, EEP_OP_WRITE, EEP_OP_ERASE, EEP_OP_ERAL, EEP_OP_WRAL };

struct Gmod2Eeprom {
   uint32_t state;
   uint32_t clk;         // last clock level
   uint32_t dout;        // data out (bit 7 of $DE00)
   uint32_t we;          // write enabled (EWEN)
   uint32_t op;          // pending write operation
   uint32_t nbits;       // CMD/DATA: bits shifted in, READ: bits still to output
   uint32_t addr;
   uint32_t word;        // READ: data out, WRITE/WRAL: data
};

static inline __attribute__((always_inline)) uint32_t gmod2_eep_read(uint32_t a) {
   return gmod2_eep ? (gmod2_eep[2 * a] << 8 | gmod2_eep[2 * a + 1]) : 0xFFFF;
}

struct Gmod2Cart;
static void gmod2_eep_io(Gmod2Cart &c, uint32_t data);

struct Gmod2Cart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *rom_ptr;
   uint32_t bank_mask;
   uint32_t rom_mask;      // ROML in 8K mode, none when the EEPROM is selected
   bool eep_cs;
   Gmod2Eeprom eep;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      if (eep_cs && !(control & IO1_MASK)) {
         data = (eep.dout && !gmod2_eep_fill_req) ? 0x80 : 0;   // busy during ERAL/WRAL
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      if (control & RW_MASK)
         return;

      uint32_t data = bus_data(control);
      bool eep_mode = (data & 0xC0) == 0x40;

      // EEPROM selected (ROM off), or deselected: out of the bus loop
      if (eep_mode || eep_cs)
         gmod2_eep_io(*this, data);

      if (!eep_mode) {
         // 8K mode (11: flash write mode, not emulated)
         rom_ptr = ef_bank_chunk(data & 0x3F & bank_mask);
         if (rom_mask != ROM_MASK_8K) {
            c64_set_exrom_game(0, 1);
            rom_mask = ROM_MASK_8K;
         }
      }
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

// core1: $DE00 write with the EEPROM selected (01), or deselected
static void __attribute__((noinline)) __time_critical_func(gmod2_eep_io)(Gmod2Cart &c, uint32_t data) {

   Gmod2Eeprom &e = c.eep;

   if ((data & 0xC0) != 0x40) {
      // chip select low: pending write operation, back to idle
      c.eep_cs = false;
      if (e.op != EEP_OP_NONE && e.we && gmod2_eep) {
         uint8_t *w = gmod2_eep + 2 * e.addr;
         if (e.op == EEP_OP_WRITE) {
            w[0] = e.word >> 8;
            w[1] = e.word;
         } else if (e.op == EEP_OP_ERASE) {
            w[0] = 0xFF;
            w[1] = 0xFF;
         } else {                         // ERAL / WRAL: by core0
            gmod2_eep_fill_req = GMOD2_FILL_REQ | (e.op == EEP_OP_ERAL ? 0xFFFF : e.word);
         }
         gmod2_eep_changes = gmod2_eep_changes + 1;
      }
      e.op = EEP_OP_NONE;
      e.dout = 1;                         // ready
      return;
   }

   if (c.rom_mask != ROM_MASK_OFF) {
      c64_set_exrom_game(1, 1);
      c.rom_mask = ROM_MASK_OFF;
   }
   if (!c.eep_cs) {
      c.eep_cs = true;
      e.state = EEP_IDLE;
      e.clk = 0;
   }

   uint32_t clk = (data >> 5) & 1;
   uint32_t di = (data >> 4) & 1;
   uint32_t rise = clk & ~e.clk;
   e.clk = clk;
   if (!rise)
      return;

   switch (e.state) {
      case EEP_IDLE:                      // start bit
         if (di) {
            e.state = EEP_CMD;
            e.nbits = 0;
            e.word = 0;
         }
         break;
      case EEP_CMD:                       // opcode (2), address (10)
      case EEP_DATA:                      // data (16)
         e.word = e.word << 1 | di;
         e.nbits++;
         if (e.state == EEP_DATA) {
            if (e.nbits == 16)
               e.state = EEP_DONE;
         } else if (e.nbits == 12) {
            uint32_t op = e.word >> 10;
            uint32_t ext = (e.word >> 8) & 3;
            e.addr = e.word & 0x3FF;
            e.state = EEP_DONE;
            e.nbits = 0;
            e.word = 0;
            if (op == 2) {                // READ: dummy 0, then the data
               e.word = gmod2_eep_read(e.addr);
               e.nbits = 16;
               e.dout = 0;
               e.state = EEP_READ;
            } else if (op == 1) {         // WRITE
               e.op = EEP_OP_WRITE;
               e.state = EEP_DATA;
            } else if (op == 3) {         // ERASE
               e.op = EEP_OP_ERASE;
            } else if (ext == 3) {        // EWEN
               e.we = 1;
            } else if (ext == 0) {        // EWDS
               e.we = 0;
            } else if (ext == 2) {        // ERAL
               e.op = EEP_OP_ERAL;
            } else {                      // WRAL
               e.op = EEP_OP_WRAL;
               e.state = EEP_DATA;
            }
         }
         break;
      case EEP_READ:                      // MSB first, then the next word
         e.dout = (e.word >> 15) & 1;
         e.word <<= 1;
         if (--e.nbits == 0) {
            e.addr = (e.addr + 1) & 0x3FF;
            e.word = gmod2_eep_read(e.addr);
            e.nbits = 16;
         }
         break;
      default:
         break;
   }
}

CART_FUNC void __time_critical_func(run_cart_gmod2)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   Gmod2Cart cart = { ef_tab[0], p->bank_mask, ROM_MASK_8K, false, {} };
   cart.eep.dout = 1;

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Prophet64 (43)
 *
 * 8K banks at ROML (8K mode), up to 32 (crt_buf + n * 8K)
 * $DF00-$DFFF write: bit 0-4 = bank, bit 5 = 1 cartridge disabled
 */
struct Prophet64Cart {

   static constexpr uint32_t COMMIT_SEL = IO2_MASK;

   uint8_t *rom;
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
      if (control & RW_MASK)
         return;
      uint8_t data = bus_data(control);
      rom_ptr = rom + ((data & 0x1F & bank_mask) << 13);
      if (data & 0x20) {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      } else {
         c64_set_exrom_game(0, 1);     // 8K
         rom_mask = ROM_MASK_8K;
      }
   }

   // cartridge on/off takes effect during the write cycle (see c64_bus.h)
   CART_METHOD void early_commit(uint32_t control) {
      if (bus_data(control) & 0x20)
         c64_set_exrom_game(1, 1);
      else
         c64_set_exrom_game(0, 1);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_X(run_cart_prophet64)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   Prophet64Cart cart = { p->crt_buf, p->crt_buf, p->bank_mask, ROM_MASK_8K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * RGCD, Hucky (57)
 *
 * 8K banks at ROML (8K mode), up to 8 (crt_buf + n * 8K)
 * $DE00-$DEFF write: bit 0-2 = bank (inverted on Hucky, subtype 1),
 *                    bit 3 = 1 cartridge disabled until reset
 */
struct RgcdCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *rom;
   uint8_t *rom_ptr;
   uint32_t bank_mask;
   uint32_t rom_mask;      // ROML when on, none when disabled
   uint8_t bank_xor;       // 7 on Hucky
   bool disabled;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if ((control & RW_MASK) || disabled)
         return;
      uint8_t data = bus_data(control);
      if (data & 0x08) {
         c64_set_exrom_game(1, 1);     // none, until reset
         rom_mask = ROM_MASK_OFF;
         disabled = true;
      } else {
         rom_ptr = rom + (((data ^ bank_xor) & 0x07 & bank_mask) << 13);
      }
   }

   CART_METHOD void early_commit(uint32_t control) {
      if (!disabled && (bus_data(control) & 0x08))
         c64_set_exrom_game(1, 1);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_Y(run_cart_rgcd)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   uint8_t bank_xor = (p->subtype == 1) ? 0x07 : 0x00;
   // bank 0 selected at reset (bank 7 of the image on Hucky)
   RgcdCart cart = { p->crt_buf, p->crt_buf + ((bank_xor & p->bank_mask) << 13),
                     p->bank_mask, ROM_MASK_8K, bank_xor, false };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Comal-80 (21)
 *
 * 16K banks, up to 8 (crt_banks)
 * $DE00-$DEFF write: bit 0-2 = bank, bit 5-7 = mode:
 *   $E0: cartridge off, $40: 8K, $80 and others: 16K
 */
struct Comal80Cart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t **banks;
   uint8_t *rom_ptr;
   uint32_t rom_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom_ptr[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void set_mode(uint8_t data) {
      switch (data & 0xE0) {
         case 0xE0:
            c64_set_exrom_game(1, 1);     // none
            rom_mask = ROM_MASK_OFF;
            break;
         case 0x40:
            c64_set_exrom_game(0, 1);     // 8K
            rom_mask = ROM_MASK_8K;
            break;
         default:
            c64_set_exrom_game(0, 0);     // 16K
            rom_mask = ROM_MASK_16K;
            break;
      }
   }

   CART_METHOD void commit(uint32_t control) {
      if (control & RW_MASK)
         return;
      uint8_t data = bus_data(control);
      rom_ptr = banks[data & 0x07];
      set_mode(data);
   }

   CART_METHOD void early_commit(uint32_t control) {
      set_mode(bus_data(control));
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_Y(run_cart_comal80)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   Comal80Cart cart = { p->crt_banks, p->crt_banks[0], ROM_MASK_16K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Westermann Learning (11)
 *
 * 16K, starts in 16K mode
 * $DF00-$DFFF read: 8K mode
 */
struct WestermannCart {

   static constexpr uint32_t COMMIT_SEL = IO2_MASK;

   uint8_t *rom;
   uint32_t rom_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if (control & RW_MASK) {
         c64_set_exrom_game(0, 1);     // 8K
         rom_mask = ROM_MASK_8K;
      }
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_westermann)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   WestermannCart cart = { p->crt_buf, ROM_MASK_16K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Simons' BASIC (4)
 *
 * 16K, starts in the mode of the CRT header
 * $DE00-$DEFF read: 8K mode, write: 16K mode
 */
struct SimonsBasicCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t *rom;
   uint32_t rom_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if (control & RW_MASK) {
         c64_set_exrom_game(0, 1);     // 8K
         rom_mask = ROM_MASK_8K;
      } else {
         c64_set_exrom_game(0, 0);     // 16K
         rom_mask = ROM_MASK_16K;
      }
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_simons_basic)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   SimonsBasicCart cart = { p->crt_buf, p->rom_mask };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Rex Utility (12)
 *
 * 8K at ROML, starts enabled
 * $DF00-$DFBF read: cartridge off, $DFC0-$DFFF read: cartridge on
 */
struct RexUtilityCart {

   static constexpr uint32_t COMMIT_SEL = IO2_MASK;

   uint8_t *rom;
   uint32_t rom_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x1FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if (!(control & RW_MASK))
         return;
      if ((bus_addr(control) & 0xC0) == 0xC0) {
         c64_set_exrom_game(0, 1);     // 8K
         rom_mask = ROM_MASK_8K;
      } else {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      }
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_X(run_cart_rex_utility)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   RexUtilityCart cart = { p->crt_buf, ROM_MASK_8K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Final Cartridge I (13)
 *
 * 16K, starts in 16K mode
 * $DE00-$DEFF access (read or write): cartridge off
 * $DF00-$DFFF access (read or write): 16K mode
 * $DE00-$DEFF / $DF00-$DFFF read: ROM $1E00-$1EFF / $1F00-$1FFF (also when off)
 */
struct FinalICart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK | IO2_MASK;

   uint8_t *rom;
   uint32_t rom_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }
      if (!(control & IO1_MASK)) {
         data = rom[0x1E00 | (bus_addr(control) & 0xFF)];
         return true;
      }
      if (!(control & IO2_MASK)) {
         data = rom[0x1F00 | (bus_addr(control) & 0xFF)];
         return true;
      }
      return false;
   }

   CART_METHOD void set_mode(uint32_t control) {
      if (!(control & IO1_MASK)) {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      } else {
         c64_set_exrom_game(0, 0);     // 16K
         rom_mask = ROM_MASK_16K;
      }
   }

   CART_METHOD void commit(uint32_t control) {
      set_mode(control);
   }

   CART_METHOD void early_commit(uint32_t control) {
      set_mode(control);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_X(run_cart_final_i)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   FinalICart cart = { p->crt_buf, ROM_MASK_16K };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Warp Speed (16)
 *
 * 16K, starts in 16K mode
 * $DE00-$DEFF write: 16K mode, $DF00-$DFFF write: cartridge off
 * $DE00-$DEFF / $DF00-$DFFF read: ROM $1E00-$1EFF / $1F00-$1FFF (also when off)
 */
struct WarpSpeedCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK | IO2_MASK;

   uint8_t *rom;
   uint32_t rom_mask;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (ROM_SEL(control, rom_mask) || (control & (IO1_MASK | IO2_MASK)) != (IO1_MASK | IO2_MASK)) {
         data = rom[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void set_mode(uint32_t control) {
      if (!(control & IO1_MASK)) {
         c64_set_exrom_game(0, 0);     // 16K
         rom_mask = ROM_MASK_16K;
      } else {
         c64_set_exrom_game(1, 1);     // none
         rom_mask = ROM_MASK_OFF;
      }
   }

   CART_METHOD void commit(uint32_t control) {
      if (!(control & RW_MASK))
         set_mode(control);
   }

   CART_METHOD void early_commit(uint32_t control) {
      set_mode(control);
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void CART_IN_SCRATCH_Y(run_cart_warp_speed)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   WarpSpeedCart cart = { p->crt_buf, ROM_MASK_16K };

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


#ifdef DEBUG
static uint8_t eapi_dbg_old;      // flash value before the last write
#endif

static uint8_t eapi_write_flash(uint16_t addr, uint8_t value) {

   // $8000-$9FFF ROML, $A000-$BFFF / $E000-$FFFF ROMH
   bool romh = addr & 0x2000;
   uint8_t *data = ef_data(ef_cur_bank, romh, true);
   if (!data)
      return REPLY_WRITE_ERROR;

   uint32_t ofs = addr & 0x1FFF;
#ifdef DEBUG
   eapi_dbg_old = data[ofs];
#endif
   uint8_t v = data[ofs] & value;      // flash can only clear bits
   data[ofs] = v;
   if (ef_psram) {
      uint8_t *cached = ef_cached_chunk(ef_cur_bank, romh);
      if (cached)
         cached[ofs] = v;
   }
   ef_save_mark(ef_cur_bank, romh);

   return (v == value) ? REPLY_EAPI_OK : REPLY_WRITE_ERROR;
}

static uint8_t eapi_erase_sector(uint16_t addr, uint8_t bank) {

   // a 64K sector covers 8 banks of ROML or ROMH
   if (bank >= EF_BANKS || (bank % 8))
      return REPLY_WRITE_ERROR;

   bool romh = (addr & 0xFF00) != 0x8000;

   for (uint8_t b = bank; b < bank + 8; b++) {
      uint8_t *data = ef_data(b, romh, false);
      if (data) {
         memset(data, 0xFF, EF_CHUNK_SIZE);
         if (ef_psram) {
            uint8_t *cached = ef_cached_chunk(b, romh);
            if (cached)
               memset(cached, 0xFF, EF_CHUNK_SIZE);
         }
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

#ifdef DEBUG
   // EasyFlash PSRAM cache misses served by core1
   static uint32_t dbg_misses;
   if (ef_psram && ef_misses != dbg_misses && !bus_scan_running()) {
      dbg_misses = ef_misses;
      printf("EasyFlash: %lu cache misses\n", (unsigned long)dbg_misses);
   }
#endif

   // write the GMod2 EEPROM file a while after the last change
   if (gmod2_active) {
      gmod2_eep_fill_pending();
      gmod2_eep_poll(gmod2_eep_changes);
   }

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


