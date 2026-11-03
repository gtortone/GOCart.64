#ifndef PSRAM_H
#define PSRAM_H

/*
 * QSPI PSRAM (APS6404L, 8 MB) on the QMI second chip select (XIP_CS1,
 * GC_PSRAM_CS_PIN in the board header).
 *
 * Mapped at PSRAM_UNCACHED_BASE (no XIP cache: a cache miss reads a whole line
 * and takes longer than an uncached byte read) and PSRAM_CACHED_BASE.
 * Configuration: QPI, read 0xEB (6 wait cycles), write 0x38, clock sys / 2
 * (125 MHz at 250 MHz). An uncached byte read takes 140-164 ns when the flash
 * is not being accessed by the other core.
 */

#include <cstdint>
#include <cstddef>

#define PSRAM_CACHED_BASE     0x11000000u
#define PSRAM_UNCACHED_BASE   0x15000000u

#define PSRAM_UNCACHED(ofs)   ((uint8_t *)(PSRAM_UNCACHED_BASE + (ofs)))

// detect and configure the PSRAM (at boot), returns its size in bytes
// (0: not found or GC_PSRAM_CS_PIN not set)
size_t psram_init(void);

// size found by psram_init()
size_t psram_size(void);

// shell command (DEBUG build), args: rest of the command line (may be NULL)
//   psram            info
//   psram test [kb]  memory test (destroys the PSRAM contents)
void psram_shell(char *args);

#endif
