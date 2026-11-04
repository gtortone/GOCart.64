#ifndef EF_SAVE_H
#define EF_SAVE_H

#include <cstdint>

/*
 * EasyFlash save file "<name>.sav" next to the CRT: the 8K chunks changed by
 * the cartridge through the EAPI (flash writes/erases). See ef_save.cpp.
 *
 * Chunk access (bank, ROML/ROMH) is provided by cartridge.cpp:
 * ef_chunk_read(), ef_chunk_write().
 */

// FNV-1a 32 bit hash of the CRT data, identity of the cartridge for the save file
uint32_t ef_save_hash(const uint8_t *data, uint32_t len);

// an EasyFlash cartridge was loaded (core1 not started yet): apply
// "<name>.sav" if it matches the CRT. file_name: CRT file (relative to the
// current directory or absolute), NULL: changes are not saved
void ef_save_start(const char *file_name, uint32_t crt_size, uint32_t crt_hash);

// a chunk was changed by the cartridge (EAPI write/erase)
void ef_save_mark(uint8_t bank, bool romh);

// true when there are changes and the cartridge stopped writing for a while
bool ef_save_due(void);

// write the save file (core1 keeps running: it only reads the chunks)
void ef_save_flush(void);

// write a pending save and forget the current save file (cartridge stopped)
void ef_save_stop(void);

/*
 * GMod2 EEPROM file "<name>.eep" (see ef_save.cpp)
 */

// GMod2 cartridge loaded (core1 not started yet): data (size bytes) filled
// from "<name>.eep", erased ($FF) if missing. file_name: CRT file, NULL: not
// saved
void gmod2_eep_start(const char *file_name, uint8_t *data, uint32_t size);

// core0, often: changes = EEPROM writes counted by core1; the file is
// written a while after the last change
void gmod2_eep_poll(uint32_t changes);

// cartridge stopped: write pending changes, forget the file
void gmod2_eep_stop(uint32_t changes);

#endif
