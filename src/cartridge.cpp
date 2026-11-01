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
#include <string.h>

uint8_t crt_buf[CRT_BUFFER_SIZE] = {};
uint8_t crt_map[EF_MAP_SIZE] = {};

// EasyFlash bank -> 8K chunk pointers (built by run_cart, read by core1)
static uint8_t *ef_roml[EF_BANKS];
static uint8_t *ef_romh[EF_BANKS];

// EasyFlash: first free 8K chunk and erased chunk (used by EAPI writes)
static uint8_t *ef_blank;
static uint32_t ef_next_chunk;

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

static void save_stop(void);

static void cart_stop(void) {

   // write a pending EasyFlash save before crt_buf is reused
   save_stop();

   multicore_reset_core1();

   c64_hold_reset();
   c64_set_exrom_game(1, 1);

   eapi_active = false;
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

/*
 * EasyFlash save file
 *
 * Changes made by the cartridge through the EAPI (flash writes/erases) are
 * kept in a file next to the CRT: "<name>.crt" -> "<name>.sav". It holds the
 * 8K chunks (bank, ROML/ROMH) that differ from the CRT, so the CRT itself is
 * never modified and deleting the .sav restores the original cartridge.
 *
 *   SAVE_HEADER, then for every chunk: SAVE_RECORD + 8K of data
 *
 * The save file is applied when the CRT is started, if its header matches the
 * CRT (data size and hash). It is written SAVE_DELAY_US after the last EAPI
 * write (the cartridge keeps running), or before another cartridge is started.
 * It is written to a temporary file first and then renamed.
 */
#define SAVE_MAGIC        "GC64SAV"
#define SAVE_VERSION      1
#define SAVE_DELAY_US     (1000 * 1000)
#define SAVE_EXT          ".sav"
#define SAVE_TMP_EXT      ".tmp"

#pragma pack(push)
#pragma pack(1)
typedef struct {
   char magic[8];          // SAVE_MAGIC
   uint16_t version;       // SAVE_VERSION
   uint16_t chunks;        // number of records
   uint32_t crt_size;      // CRT data size (chips)
   uint32_t crt_hash;      // FNV-1a of the CRT data (packed 8K chunks)
} SAVE_HEADER;

typedef struct {
   uint8_t bank;           // EasyFlash bank (0-63)
   uint8_t romh;           // 0: ROML, 1: ROMH
   uint16_t reserved;
} SAVE_RECORD;
#pragma pack(pop)

static char save_path[256 + 64];                  // "" = no save file
static uint32_t save_crt_size;
static uint32_t save_crt_hash;
static uint8_t save_modified[EF_BANKS * 2 / 8];   // chunks different from the CRT
static bool save_pending;                         // changes not written yet
static uint64_t save_last_change;

static inline bool save_is_modified(uint8_t bank, bool romh) {
   uint8_t n = bank * 2 + romh;
   return save_modified[n >> 3] & (1 << (n & 7));
}

// FNV-1a 32 bit
static uint32_t save_hash(const uint8_t *data, uint32_t len) {
   uint32_t h = 2166136261u;
   while (len--) {
      h ^= *data++;
      h *= 16777619u;
   }
   return h;
}

// "<dir>/<name>.crt" -> "<dir>/<name>.sav" (absolute: the directory may change)
static bool save_set_path(const char *file_name) {

   save_path[0] = 0;
   size_t max = sizeof(save_path) - sizeof(SAVE_TMP_EXT) - sizeof(SAVE_EXT);

   if (file_name[0] == '/' || strchr(file_name, ':')) {
      if (strlen(file_name) >= max)
         return false;
      strcpy(save_path, file_name);
   } else {
      if (!dir_current(save_path, max))
         return false;
      size_t len = strlen(save_path);
      if (len + 1 + strlen(file_name) >= max) {
         save_path[0] = 0;
         return false;
      }
      if (len && save_path[len - 1] != '/')
         strcat(save_path, "/");
      strcat(save_path, file_name);
   }

   // replace the extension
   char *slash = strrchr(save_path, '/');
   char *dot = strrchr(save_path, '.');
   if (dot && (!slash || dot > slash))
      *dot = 0;
   strcat(save_path, SAVE_EXT);
   return true;
}

static uint8_t *eapi_get_chunk(uint8_t bank, bool romh);

// apply the save file to the cartridge data (before core1 starts)
static void save_load(void) {

   FIL file;
   if (!file_open(&file, save_path, FA_READ))
      return;     // no save file yet

   SAVE_HEADER h;
   bool ok = file_read(&file, &h, sizeof(h)) == sizeof(h) &&
             memcmp(h.magic, SAVE_MAGIC, sizeof(h.magic)) == 0 &&
             h.version == SAVE_VERSION;

   if (ok && (h.crt_size != save_crt_size || h.crt_hash != save_crt_hash)) {
      printf("W: %s is for another cartridge, ignored\n", save_path);
      file_close(&file);
      return;
   }

   uint16_t n = 0;
   for (; ok && n < h.chunks; n++) {
      SAVE_RECORD r;
      uint8_t *chunk;
      ok = file_read(&file, &r, sizeof(r)) == sizeof(r) && r.bank < EF_BANKS &&
           (chunk = eapi_get_chunk(r.bank, r.romh)) != NULL &&
           file_read(&file, chunk, EF_CHUNK_SIZE) == EF_CHUNK_SIZE;
      if (ok) {
         uint8_t i = r.bank * 2 + (r.romh ? 1 : 0);
         save_modified[i >> 3] |= 1 << (i & 7);
      }
   }

   file_close(&file);

   if (ok)
      printf("EasyFlash: %d chunks loaded from %s\n", n, save_path);
   else
      printf("E: %s not valid\n", save_path);
}

// a chunk was changed by the cartridge
static void save_mark(uint8_t bank, bool romh) {
   uint8_t i = bank * 2 + (romh ? 1 : 0);
   save_modified[i >> 3] |= 1 << (i & 7);
   save_pending = true;
   save_last_change = time_us_64();
}

static bool save_write_rec(FIL *file, uint8_t bank, bool romh) {
   SAVE_RECORD r = { bank, (uint8_t)(romh ? 1 : 0), 0 };
   uint8_t *chunk = romh ? ef_romh[bank] : ef_roml[bank];
   return file_write(file, &r, sizeof(r)) == sizeof(r) &&
          file_write(file, chunk, EF_CHUNK_SIZE) == EF_CHUNK_SIZE;
}

// write all the changed chunks (core1 keeps running: it only reads them)
static void save_flush(void) {

   save_pending = false;
   if (!save_path[0])
      return;

   char tmp_path[sizeof(save_path)];
   strcpy(tmp_path, save_path);
   strcat(tmp_path, SAVE_TMP_EXT);

   SAVE_HEADER h;
   memcpy(h.magic, SAVE_MAGIC, sizeof(h.magic));
   h.version = SAVE_VERSION;
   h.chunks = 0;
   h.crt_size = save_crt_size;
   h.crt_hash = save_crt_hash;
   for (uint8_t bank = 0; bank < EF_BANKS; bank++)
      h.chunks += save_is_modified(bank, false) + save_is_modified(bank, true);

   FIL file;
   bool ok = file_open(&file, tmp_path, FA_WRITE | FA_CREATE_ALWAYS) &&
             file_write(&file, &h, sizeof(h)) == sizeof(h);

   for (uint8_t bank = 0; ok && bank < EF_BANKS; bank++) {
      for (uint8_t romh = 0; ok && romh < 2; romh++) {
         if (save_is_modified(bank, romh))
            ok = save_write_rec(&file, bank, romh);
      }
   }

   ok = file_close(&file) && ok;

   // replace the old save file
   if (ok) {
      f_unlink(save_path);
      ok = f_rename(tmp_path, save_path) == FR_OK;
   }

   if (ok)
      printf("EasyFlash: %d chunks saved to %s\n", h.chunks, save_path);
   else
      printf("E: failed to write %s\n", save_path);
}

// write a pending save and forget the current save file
static void save_stop(void) {
   if (save_pending)
      save_flush();
   save_path[0] = 0;
   save_pending = false;
   memset(save_modified, 0, sizeof(save_modified));
}

// set up the save file of an EasyFlash cartridge just loaded
static void save_start(const char *file_name, uint32_t crt_size) {
   save_crt_size = crt_size;
   if (file_name && save_set_path(file_name))
      save_load();
   else
      printf("W: EasyFlash changes will not be saved\n");
}

// EAPI driver: $F800-$FAFF in bank 0 ROMH, $B800 in the 16K launcher image
#define EAPI_OFFSET_ROMH       0x1800
#define EAPI_OFFSET_LAUNCHER   0x3800
#define EAPI_SIZE              0x300

// EasyFlash: resolve the chunk map built by CRTParser into pointer tables.
// Missing chips point to an erased (0xFF) chunk, like an empty flash.
static void easyflash_build_tables(uint8_t chunks) {

   uint8_t *blank;

   if ((uint32_t)(chunks + 1) * EF_CHUNK_SIZE <= sizeof(crt_buf)) {
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
   save_crt_hash = save_hash(crt_buf, (uint32_t)chunks * EF_CHUNK_SIZE);

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
         args.crt_banks = crt_banks;
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
         save_start(file_name, crt->getSize());     // apply "<name>.sav", if any
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
 * Normal cartridge: 8K, 16K, Ultimax
 */
struct NormalCart {

   static constexpr uint32_t COMMIT_SEL = 0;   // no registers

   uint8_t *rom;
   bool ultimax;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (bus_rom_sel(control)) {
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
   NormalCart cart = { p->crt_buf, p->ultimax };

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

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (!(control & ROML_MASK)) {
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
      } else {
         c64_set_exrom_game(1, 1);     // none
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
   MagicDeskCart cart = { p->crt_banks, p->crt_banks[0], p->bank_mask };

   c64_reset();
   /* uint32_t irqstatus = */ save_and_disable_interrupts();

   c64_bus_loop(cart);
}

//

/*
 * Ocean (5)
 *
 * $DE00 write: 16K slot (ROML bank n, ROMH bank n+16)
 */
struct OceanCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK;

   uint8_t **banks;
   uint8_t *rom_ptr;

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (bus_rom_sel(control)) {
         data = rom_ptr[bus_addr(control) & 0x3FFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {
      if (!(control & RW_MASK) && (bus_addr(control) == 0xDE00))
         rom_ptr = banks[bus_data(control) % 16];
   }

   CART_METHOD bool vic_enabled(void) { return false; }

   CART_METHOD uint8_t vic_read(uint32_t addr) { (void)addr; return 0; }
};

CART_FUNC void __time_critical_func(run_cart_ocean)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();
   OceanCart cart = { p->crt_banks, p->crt_banks[0] };

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

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (!(control & ROML_MASK)) {
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
      } else {
         c64_set_exrom_game(1, 1);     // none
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
   FunPlayCart cart = { p->crt_banks, p->crt_banks[0] };

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

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (bus_rom_sel(control)) {
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
      } else {
         c64_set_exrom_game(0, 0);     // 16K
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
   SuperGamesCart cart = { p->crt_banks, p->crt_banks[0], false };

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

struct EasyFlashCart {

   static constexpr uint32_t COMMIT_SEL = IO1_MASK | IO2_MASK;

   uint8_t * const *roml_tab;
   uint8_t * const *romh_tab;
   uint8_t *roml_ptr;
   uint8_t *romh_ptr;
   bool ultimax;
   uint8_t ram[256];

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (bus_rom_sel(control)) {
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

};

CART_FUNC void __time_critical_func(run_cart_easyflash)(void) {

   /* core1_args_t *p = */ multicore_fifo_pop_blocking();

   // EasyFlash boots in Ultimax mode (set by run_cart from CRT header), bank 0
   EasyFlashCart cart = { ef_roml, ef_romh, ef_roml[0], ef_romh[0], true, {} };

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

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (!(control & ROMH_MASK)) {
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
   CleanCart cart = { p->crt_buf, true };

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

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {

      if (bus_rom_sel(control)) {
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
            } else {
               c64_set_exrom_game(1, 1);     // none
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
   KffCart cart = { p->crt_buf, true };

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

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {

      if (bus_rom_sel(control)) {
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
   Ef3Cart cart = { p->crt_buf, true };

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
 * The bank is the one selected in $DE00 before the command.
 * Changes are kept in RAM only (lost on reset/power off).
 */

// EAPI protocol (see launcher/eapi-kung_fu_flash.s)
#define CMD_EAPI_INIT       0x01
#define CMD_WRITE_FLASH     0x02
#define CMD_ERASE_SECTOR    0x03
#define REPLY_EAPI_OK       0x00
#define REPLY_WRITE_ERROR   0x02

// get the chunk of an EasyFlash bank half, allocating it if it is the
// shared erased chunk (copy on write); returns NULL if no memory is left
static uint8_t *eapi_get_chunk(uint8_t bank, bool romh) {

   uint8_t **tab = romh ? ef_romh : ef_roml;
   uint8_t *chunk = tab[bank];

   if (chunk == ef_blank) {
      if ((ef_next_chunk + 1) * EF_CHUNK_SIZE > sizeof(crt_buf))
         return NULL;
      chunk = crt_buf + ef_next_chunk * EF_CHUNK_SIZE;
      ef_next_chunk++;
      memset(chunk, 0xFF, EF_CHUNK_SIZE);
      // core1 picks it up at the next $DE00 write (EAPI always sets the bank)
      tab[bank] = chunk;
   }

   return chunk;
}

static uint8_t eapi_write_flash(uint16_t addr, uint8_t value) {

   // $8000-$9FFF ROML, $A000-$BFFF / $E000-$FFFF ROMH
   uint8_t *chunk = eapi_get_chunk(ef_cur_bank, addr & 0x2000);
   if (!chunk)
      return REPLY_WRITE_ERROR;

   uint8_t *dest = chunk + (addr & 0x1FFF);
   *dest &= value;      // flash can only clear bits
   save_mark(ef_cur_bank, addr & 0x2000);

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
         save_mark(b, romh);
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

static void eapi_dbg_report(void) {
   if (eapi_dbg_writes)
      printf("EAPI: %lu bytes written\n", (unsigned long)eapi_dbg_writes);
   eapi_dbg_writes = 0;
}
#endif

void cart_poll(void) {

   // write the EasyFlash save file when the cartridge stopped writing
   if (save_pending && time_us_64() - save_last_change > SAVE_DELAY_US) {
#ifdef DEBUG
      eapi_dbg_report();
#endif
      save_flush();
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

   uint8_t reply = REPLY_EAPI_OK;

   if (cmd == CMD_WRITE_FLASH || cmd == CMD_ERASE_SECTOR) {
      uint8_t args[3];    // addr_lo, addr_hi, value/bank
      if (!ef3_receive_data(args, sizeof(args)))
         return;

      uint16_t addr = args[0] | (args[1] << 8);
      reply = (cmd == CMD_WRITE_FLASH) ? eapi_write_flash(addr, args[2])
                                       : eapi_erase_sector(addr, args[2]);
   }

   // reply first: the EAPI init waits for it only a few ms
   ef3_putc(reply);

#ifdef DEBUG
   // writes are only counted: printing every byte would slow down the EAPI
   if (cmd == CMD_WRITE_FLASH) {
      eapi_dbg_writes++;
      if (reply != REPLY_EAPI_OK)
         printf("EAPI: write in bank %u failed\n", ef_cur_bank);
   } else {
      eapi_dbg_report();
      if (cmd == CMD_ERASE_SECTOR)
         printf("EAPI: erase -> %u\n", reply);
      else
         printf("EAPI: command %u\n", cmd);
   }
#endif
}
