/*
 * Cartridge chunk tables and the PSRAM chunk cache (see ef_cache.h)
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"

#include "cartridge.h"
#include "cartridge_int.h"
#include "c64_interface.h"
#include "board.h"
#include "ef_save.h"
#include "psram.h"
#include "bus_diag.h"
#include "ef_cache.h"

// crt_map index -> 8K chunk pointer (built by run_cart, read by core1):
// EasyFlash ROML banks, then ROMH banks; Ocean, C64 Game System, Magic Desk:
// 8K banks (up to EF_MAP_SIZE). NULL: chunk in the PSRAM, not in the cache
// (see the PSRAM chunk cache)
uint8_t *ef_tab[EF_MAP_SIZE];

// EasyFlash: first free 8K chunk and erased chunk (used by EAPI writes)
static uint8_t *ef_blank;
static uint32_t ef_next_chunk;

// identity of the EasyFlash cartridge data, checked by the save file
uint32_t ef_crt_hash;

bool ef_psram;                                  // cartridge data in the PSRAM
static uint8_t ef_slot_chunk[EF_SLOTS];          // slot -> chunk, EF_CHUNK_NONE: free
static uint8_t ef_chunk_slot[EF_MAP_SIZE];       // chunk -> slot, EF_NO_SLOT
static uint32_t ef_victim;                       // next slot to replace (round robin)
// slots used by the cache (<= EF_SLOTS: GMod2 keeps the last one for its
// EEPROM), set by core0 before core1 starts
uint32_t ef_nslots = EF_SLOTS;
volatile uint32_t ef_misses;              // cache misses served by core1
volatile uint32_t ef_last_miss_us;        // timer (low 32 bits) at the last miss

// point every bank half that uses the chunk to ptr
static void __attribute__((noinline)) __scratch_y("ef_cache_map") ef_cache_map(uint8_t chunk, uint8_t *ptr) {
   for (uint32_t i = 0; i < EF_MAP_SIZE; i++) {
      if (crt_map[i] == chunk)
         ef_tab[i] = ptr;
   }
}

// copy a chunk from the PSRAM to a slot (free, else round robin: no work
// is done on cache hits), never the slot keep nor a bank 0 chunk; returns
// the slot
static uint32_t __attribute__((noinline)) __scratch_y("ef_cache_load") ef_cache_load(uint8_t chunk, uint32_t keep) {

   uint32_t slot = EF_SLOTS;
   for (uint32_t n = 0; n < ef_nslots; n++) {
      if (ef_slot_chunk[n] == EF_CHUNK_NONE && n != keep) {
         slot = n;
         break;
      }
   }
   if (slot == EF_SLOTS) {
      slot = ef_victim;
      for (uint32_t n = 0; n < ef_nslots; n++) {
         uint8_t c = ef_slot_chunk[slot];
         if (slot != keep && c != crt_map[0] && c != crt_map[EF_BANKS])
            break;
         slot = (slot + 1) % ef_nslots;
      }
      ef_victim = (slot + 1) % ef_nslots;
   }

   uint8_t old = ef_slot_chunk[slot];
   if (old != EF_CHUNK_NONE) {
      ef_chunk_slot[old] = EF_NO_SLOT;
      ef_cache_map(old, NULL);
   }

   const volatile uint32_t *src = (const volatile uint32_t *)EF_PSRAM_CHUNK(chunk);
   uint32_t *dst = (uint32_t *)EF_SLOT(slot);
   for (uint32_t i = 0; i < EF_CHUNK_SIZE / 4; i++)
      dst[i] = src[i];
   // check: read again, a word that differs is read until two reads agree
   for (uint32_t i = 0; i < EF_CHUNK_SIZE / 4; i++) {
      uint32_t v = src[i];
      if (v != dst[i]) {
         uint32_t w;
         for (int n = 0; n < 8 && (w = src[i]) != v; n++)
            v = w;
         dst[i] = v;
      }
   }

   ef_slot_chunk[slot] = chunk;
   ef_chunk_slot[chunk] = slot;
   ef_cache_map(chunk, EF_SLOT(slot));
   return slot;
}

// slot of a cached chunk pointer (EF_SLOTS: erased chunk)
static inline __attribute__((always_inline)) uint32_t ef_slot_of(const uint8_t *ptr) {
   return (uint32_t)(ptr - crt_buf) / EF_CHUNK_SIZE;
}

// core1: a bank register write selected chunks not in the cache: crt_map
// indexes i0 and i1 (EF_MAP_SIZE: none), called at the end of the write
// cycle
void __attribute__((noinline)) __scratch_x("ef_cache_miss") ef_cache_miss2(uint32_t i0, uint32_t i1) {

   c64_dma_assert();
   c64_dma_bus_start();

   uint32_t keep = EF_SLOTS;
   if (!ef_tab[i0])
      keep = ef_cache_load(crt_map[i0], EF_SLOTS);
   else
      keep = ef_slot_of(ef_tab[i0]);
   if (i1 < EF_MAP_SIZE && !ef_tab[i1])
      ef_cache_load(crt_map[i1], keep);

   // release at a PHI2 falling edge: the bus loop continues with the VIC-II
   // phase, the 6510 repeats its read in the next PHI2 phase.
   // c64_dma_bus_stop() is quick (the pins keep the PIO function): the bus
   // loop must be back before the next PHI2 rising edge
   wait_high(PHI2);
   wait_low(PHI2);
   c64_dma_bus_stop();
   c64_dma_release();
   ef_misses = ef_misses + 1;
   ef_last_miss_us = timer_hw->timerawl;
}

// core0: PSRAM chunk of a bank half, allocated (erased) if missing and alloc;
// NULL if missing
static uint8_t *ef_psram_chunk(uint8_t bank, bool romh, bool alloc) {
   uint8_t *map = &crt_map[(romh ? EF_BANKS : 0) + bank];
   if (*map == EF_CHUNK_NONE) {
      if (!alloc || ef_next_chunk >= EF_MAP_SIZE)
         return NULL;
      uint8_t chunk = ef_next_chunk++;
      memset(EF_PSRAM_CHUNK(chunk), 0xFF, EF_CHUNK_SIZE);
      *map = chunk;
      // it pointed to the erased chunk: core1 loads it at the next $DE00
      // write (EAPI always sets the bank)
      (romh ? ef_romh : ef_roml)[bank] = NULL;
   }
   return EF_PSRAM_CHUNK(*map);
}

// core0: cached copy of the PSRAM chunk of a bank half, NULL if not cached
uint8_t *ef_cached_chunk(uint8_t bank, bool romh) {
   uint8_t chunk = crt_map[(romh ? EF_BANKS : 0) + bank];
   if (chunk == EF_CHUNK_NONE || ef_chunk_slot[chunk] == EF_NO_SLOT)
      return NULL;
   return EF_SLOT(ef_chunk_slot[chunk]);
}

// core0, before core1 starts: empty cache, bank 0 first, then the chunks in
// load order while slots are free
void ef_cache_fill(uint8_t chunks) {

   memset(ef_slot_chunk, EF_CHUNK_NONE, sizeof(ef_slot_chunk));
   memset(ef_chunk_slot, EF_NO_SLOT, sizeof(ef_chunk_slot));
   ef_victim = 0;
   ef_misses = 0;

   // bank 0 first (EasyFlash ROML/ROMH; 8K bank types: banks 0 and 64);
   // a chunk can be shared by more banks: loaded once
   uint32_t keep = EF_SLOTS;
   if (crt_map[0] != EF_CHUNK_NONE)
      keep = ef_cache_load(crt_map[0], EF_SLOTS);
   uint8_t c1 = crt_map[EF_BANKS];
   if (c1 != EF_CHUNK_NONE && ef_chunk_slot[c1] == EF_NO_SLOT)
      ef_cache_load(c1, keep);

   for (uint8_t c = 0; c < chunks; c++) {
      if (ef_chunk_slot[c] != EF_NO_SLOT)
         continue;
      bool free_slot = false;
      for (uint32_t n = 0; n < ef_nslots; n++)
         free_slot |= ef_slot_chunk[n] == EF_CHUNK_NONE;
      if (!free_slot)
         break;
      ef_cache_load(c, EF_SLOTS);
   }

   uint32_t cached = 0;
   for (uint32_t n = 0; n < ef_nslots; n++)
      cached += ef_slot_chunk[n] != EF_CHUNK_NONE;
   printf("PSRAM: %lu of %d chunks in the cache\n", (unsigned long)cached, chunks);
}

// EAPI driver: $F800-$FAFF in bank 0 ROMH, $B800 in the 16K launcher image
#define EAPI_OFFSET_ROMH       0x1800
#define EAPI_OFFSET_LAUNCHER   0x3800
#define EAPI_SIZE              0x300

// EasyFlash cartridge in the PSRAM (see the EasyFlash PSRAM cache): pointer
// tables (missing chips: erased chunk, the others: not cached yet), hash and
// EAPI replacement on the PSRAM data. The cache is filled by ef_cache_fill()
// after the save file has been applied.
static void easyflash_build_tables_psram(uint8_t chunks) {

   uint8_t *blank = EF_SLOT(EF_SLOTS);
   memset(blank, 0xFF, EF_CHUNK_SIZE);
   ef_blank = blank;
   ef_next_chunk = chunks;
   ef_cur_bank = 0;

   for (int bank = 0; bank < EF_BANKS; bank++) {
      ef_roml[bank] = (crt_map[bank] == EF_CHUNK_NONE) ? blank : NULL;
      ef_romh[bank] = (crt_map[EF_BANKS + bank] == EF_CHUNK_NONE) ? blank : NULL;
   }

   printf("EasyFlash: %d chunks (8K) in the PSRAM\n", chunks);

   ef_crt_hash = ef_save_hash(EF_PSRAM_CHUNK(0), (uint32_t)chunks * EF_CHUNK_SIZE);

   uint8_t c = crt_map[EF_BANKS];
   uint8_t *eapi = (c == EF_CHUNK_NONE) ? NULL : EF_PSRAM_CHUNK(c) + EAPI_OFFSET_ROMH;
   if (eapi && memcmp(eapi, "eapi", 4) == 0 &&
       memcmp(CRT_LAUNCHER + EAPI_OFFSET_LAUNCHER, "eapi", 4) == 0) {
      memcpy(eapi, CRT_LAUNCHER + EAPI_OFFSET_LAUNCHER, EAPI_SIZE);
      bool ok = memcmp(eapi, CRT_LAUNCHER + EAPI_OFFSET_LAUNCHER, EAPI_SIZE) == 0;
      printf("EasyFlash: EAPI replaced%s\n", ok ? "" : " (PSRAM check FAILED)");
   } else {
      printf("EasyFlash: no EAPI found, flash writes not supported\n");
   }
}

// EasyFlash: resolve the chunk map built by CRTParser into pointer tables.
// Missing chips point to an erased (0xFF) chunk, like an empty flash.
void easyflash_build_tables(uint8_t chunks) {

   uint8_t *blank;

#if defined(DEBUG) && EF_DEBUG_CPU_TRACE
   // the CPU trace uses the end of crt_buf, if the cartridge leaves it free
   cpu_trace_reset(ef_psram ||
                   (uint32_t)(chunks + 1) * EF_CHUNK_SIZE <= CRT_BUFFER_SIZE - CPU_TRACE_BYTES);
   printf("EasyFlash: CPU trace %s\n", cpu_trace_on ? "on" : "off (cartridge too large)");
#endif

   if (ef_psram) {
      easyflash_build_tables_psram(chunks);
      return;
   }

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

// 8K bank cartridges (Ocean, C64 Game System, Magic Desk): bank pointers
// from the chunk map built by CRTParser (parse8KPackedChip). Erased or
// missing banks point to an erased chunk. In the PSRAM: the chunks are
// loaded into the cache when selected (see the PSRAM chunk cache).
void packed_build_tables(uint8_t chunks, const char *name) {

   uint8_t erased = 0;
   for (int i = 0; i < EF_MAP_SIZE; i++)
      erased += crt_map[i] == EF_CHUNK_NONE;

   if (ef_psram) {
      uint8_t *blank = EF_SLOT(EF_SLOTS);
      memset(blank, 0xFF, EF_CHUNK_SIZE);
      ef_blank = blank;
      for (int i = 0; i < EF_MAP_SIZE; i++)
         ef_tab[i] = (crt_map[i] == EF_CHUNK_NONE) ? blank : NULL;
      printf("%s: %d chunks (8K) in the PSRAM\n", name, chunks);
      ef_cache_fill(chunks);
      return;
   }

   uint8_t *blank = NULL;
   if ((uint32_t)(chunks + 1) * EF_CHUNK_SIZE <= sizeof(crt_buf)) {
      blank = crt_buf + (uint32_t)chunks * EF_CHUNK_SIZE;
      memset(blank, 0xFF, EF_CHUNK_SIZE);
   }
   for (int i = 0; i < EF_MAP_SIZE; i++) {
      uint8_t c = crt_map[i];
      // no room for an erased chunk: the buffer is full of unique banks
      ef_tab[i] = (c == EF_CHUNK_NONE) ? (blank ? blank : crt_buf) :
                                         crt_buf + (uint32_t)c * EF_CHUNK_SIZE;
   }

   printf("%s: %d chunks (8K) used%s\n", name, chunks,
          (erased && !blank) ? " (W: no erased chunk)" : "");
}

// get the chunk of an EasyFlash bank half, allocating it if it is the
// shared erased chunk (copy on write); returns NULL if no memory is left
// (cartridge in SRAM)
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

// data of a bank half for core0 (cartridge data: SRAM chunk or PSRAM chunk),
// NULL if missing and not alloc
uint8_t *ef_data(uint8_t bank, bool romh, bool alloc) {
   if (ef_psram)
      return ef_psram_chunk(bank, romh, alloc);
   if (alloc)
      return eapi_get_chunk(bank, romh);
   uint8_t *chunk = romh ? ef_romh[bank] : ef_roml[bank];
   return (chunk == ef_blank) ? NULL : chunk;
}

bool ef_chunk_read(uint8_t bank, bool romh, uint32_t ofs, uint8_t *dst, uint32_t len) {
   const uint8_t *data = ef_data(bank, romh, false);
   if (data)
      memcpy(dst, data + ofs, len);
   else
      memset(dst, 0xFF, len);
   return true;
}

bool ef_chunk_write(uint8_t bank, bool romh, uint32_t ofs, const uint8_t *src, uint32_t len) {
   uint8_t *data = ef_data(bank, romh, true);
   if (!data)
      return false;
   memcpy(data + ofs, src, len);
   if (ef_psram) {
      uint8_t *cached = ef_cached_chunk(bank, romh);
      if (cached)
         memcpy(cached + ofs, src, len);
   }
   return true;
}
