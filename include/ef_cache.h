#ifndef EF_CACHE_H
#define EF_CACHE_H

/*
 * Cartridge chunk tables (8K chunks of crt_buf, see CRTParser) and the PSRAM
 * chunk cache (ef_cache.cpp)
 */

#include <stdint.h>
#include <string.h>
#include "cartridge.h"
#include "psram.h"
#include "ef_save.h"
#include "bus_diag.h"

/*
 * EasyFlash PSRAM cache (PSRAM chunk cache)
 *
 * Also used by the 8K bank cartridges (Ocean, C64 Game System, Magic Desk):
 * their banks are chunks too (crt_map[bank], ef_tab[bank]), a bank register
 * write that selects a chunk not in the cache is served the same way
 * (ef_bank_chunk). Below, "bank 0" is crt_map[0] and crt_map[EF_BANKS].
 *
 * An EasyFlash CRT larger than crt_buf (up to 1 MB) is loaded into the PSRAM:
 * chunk n (8K, same packing as in crt_buf, see CRTParser) at PSRAM offset
 * n * 8K. The PSRAM is too slow to be read in a bus cycle (the data must be
 * on the bus ~100 ns after ROML/ROMH), so crt_buf becomes a cache of
 * EF_SLOTS chunks, the slot after the last one holds the erased chunk.
 *
 * ef_roml/ef_romh point to the cached chunk or are NULL. A $DE00 write that
 * selects a bank with a chunk not in the cache (core1, commit at the end of
 * the write cycle) asserts the DMA line: the 6510 stops at the next cycle,
 * that is a read. While it is stopped the PIO drives the address bus and R/W
 * (see dma_bus.pio). The chunk is copied from the PSRAM to a slot (free, else
 * round robin, never the bank 0 chunks: the EasyFlash menus and loaders run
 * from bank 0 and come back to it after every access to another bank), then
 * DMA is released at a PHI2 falling edge and the 6510 repeats its read. The
 * VIC-II keeps running (an Ultimax ROMH fetch during the copy reads garbage).
 * A copy takes ~0.35 ms per 8K (~0.7 ms with the check). Nothing is done on
 * a cache hit: the $DE00 write is handled as with the cartridge in SRAM.
 *
 * The cache functions used by core1 are in SCRATCH_X/Y (main RAM is full).
 *
 * The PSRAM holds the cartridge data: EAPI writes (core0) go to the PSRAM and
 * to the cached copy, if any. Core0 and core1 do not change the cache at the
 * same time: EAPI commands are sent with the bank already selected (no $DE00
 * write while core0 serves them), the save file is read from the PSRAM.
 */
#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
// the CPU trace uses the end of crt_buf (see bus_diag.h)
#define EF_SLOTS              ((CRT_BUFFER_SIZE - CPU_TRACE_BYTES) / EF_CHUNK_SIZE - 1)
#else
#define EF_SLOTS              (CRT_BUFFER_SIZE / EF_CHUNK_SIZE - 1)
#endif
#define EF_SLOT(n)            (crt_buf + (uint32_t)(n) * EF_CHUNK_SIZE)
#define EF_PSRAM_CHUNK(c)     PSRAM_UNCACHED((uint32_t)(c) * EF_CHUNK_SIZE)
#define EF_PSRAM_MAX          ((uint32_t)EF_MAP_SIZE * EF_CHUNK_SIZE)
#define EF_NO_SLOT            0xFF

// end of crt_buf used by the CPU trace, if enabled (see bus_diag.h)
#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
#define EF_BUF_LIMIT   (CRT_BUFFER_SIZE - (cpu_trace_on ? CPU_TRACE_BYTES : 0))
#else
#define EF_BUF_LIMIT   CRT_BUFFER_SIZE
#endif

extern uint8_t crt_buf[CRT_BUFFER_SIZE];
extern uint8_t crt_map[EF_MAP_SIZE];

// crt_map index -> 8K chunk pointer (built by run_cart, read by core1):
// EasyFlash ROML banks, then ROMH banks; Ocean, C64 Game System, Magic Desk,
// GMod2: 8K banks (up to EF_MAP_SIZE). NULL: chunk in the PSRAM, not in the
// cache
extern uint8_t *ef_tab[EF_MAP_SIZE];
static uint8_t ** const ef_roml = ef_tab;
static uint8_t ** const ef_romh = ef_tab + EF_BANKS;

// identity of the EasyFlash cartridge data, checked by the save file
extern uint32_t ef_crt_hash;

extern bool ef_psram;                     // cartridge data in the PSRAM
// slots used by the cache (<= EF_SLOTS: GMod2 keeps the last one for its
// EEPROM), set by core0 before core1 starts
extern uint32_t ef_nslots;
extern volatile uint32_t ef_misses;       // cache misses served by core1
extern volatile uint32_t ef_last_miss_us; // timer (low 32 bits) at the last miss

// core1: a bank register write selected chunks not in the cache: crt_map
// indexes i0 and i1 (EF_MAP_SIZE: none), called at the end of the write
// cycle (SCRATCH_X)
void ef_cache_miss2(uint32_t i0, uint32_t i1);

// EasyFlash bank: ROML and ROMH
static inline __attribute__((always_inline)) void ef_cache_miss(uint8_t bank) {
   ef_cache_miss2(bank, EF_BANKS + bank);
}

// 8K bank (Ocean, C64 Game System, Magic Desk): pointer to its chunk, loaded
// into the cache first if needed
static inline __attribute__((always_inline)) uint8_t *ef_bank_chunk(uint32_t bank) {
   uint8_t *p = ef_tab[bank];
   if (__builtin_expect(!p, 0)) {
      ef_cache_miss2(bank, EF_MAP_SIZE);
      p = ef_tab[bank];
   }
   return p;
}

// core0, before core1 starts: empty cache, bank 0 first, then the chunks in
// load order while slots are free
void ef_cache_fill(uint8_t chunks);

// core0: cached copy of the PSRAM chunk of a bank half, NULL if not cached
uint8_t *ef_cached_chunk(uint8_t bank, bool romh);

// data of a bank half for core0 (cartridge data: SRAM chunk or PSRAM chunk),
// NULL if missing and not alloc
uint8_t *ef_data(uint8_t bank, bool romh, bool alloc);

// cartridge stopped: cache unused, all the slots available again
static inline void ef_cache_stop(void) {
   ef_psram = false;
   ef_nslots = EF_SLOTS;
}

// EasyFlash: resolve the chunk map built by CRTParser into pointer tables
// (in the PSRAM: tables of the cache, filled by ef_cache_fill())
void easyflash_build_tables(uint8_t chunks);

// 8K bank cartridges: bank pointers from the chunk map (in the PSRAM: the
// cache is filled too)
void packed_build_tables(uint8_t chunks, const char *name);

/*
 * Reader for a CRT loaded into the PSRAM: the data goes through a buffer in
 * SRAM (the SD card driver is not used with XIP addresses)
 */
class PsramBounceReader : public IDataReader {
public:
   PsramBounceReader(IDataReader &r, uint8_t *buf, uint32_t buf_size) :
      r(r), buf(buf), buf_size(buf_size) {}

   bool read(uint8_t *dst, size_t n) override {
      if ((uint32_t)dst < 0x10000000u || (uint32_t)dst >= 0x20000000u)
         return r.read(dst, n);
      while (n) {
         uint32_t k = n < buf_size ? n : buf_size;
         if (!r.read(buf, k))
            return false;
         memcpy(dst, buf, k);
         // read back, write again the bytes that differ
         for (uint32_t i = 0; i < k; i++) {
            const volatile uint8_t *p = dst + i;
            if (*p != buf[i]) {
               for (int t = 0; t < 8 && *p != buf[i]; t++)
                  *(volatile uint8_t *)p = buf[i];
            }
         }
         // data written to the PSRAM, in order (checked after the load)
         if (dst != PSRAM_UNCACHED(written))
            contiguous = false;
         for (uint32_t i = 0; i < k; i++) {
            hash ^= buf[i];
            hash *= 16777619u;
         }
         written += k;
         dst += k;
         n -= k;
      }
      return true;
   }

   // after the load: the PSRAM holds the data written, false if it differs.
   // Checked when written in order from offset 0 (EasyFlash); the 8K bank
   // types write an erased or repeated bank again over the same chunk: then
   // only the read back of every write (above) applies
   bool check(void) const {
      return !contiguous || ef_save_hash(PSRAM_UNCACHED(0), written) == hash;
   }

   void restart(void) {
      written = 0;
      hash = 2166136261u;
      contiguous = true;
   }

   uint32_t bytes(void) const { return written; }

   bool seek(size_t pos) override { return r.seek(pos); }
   size_t tell() const override { return r.tell(); }
   bool eof() const override { return r.eof(); }
   size_t size() const override { return r.size(); }

private:
   IDataReader &r;
   uint8_t *buf;
   uint32_t buf_size;
   uint32_t written = 0;
   uint32_t hash = 2166136261u;      // FNV-1a of the data written
   bool contiguous = true;
};

#endif
