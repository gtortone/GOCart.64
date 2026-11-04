/*
 * WeAct Studio RP2350B Core with the 8 MB QSPI PSRAM (APS6404L) mounted
 *
 * Same as weact_rp2350b_core.h, plus the PSRAM chip select. The PSRAM is used
 * for the cartridge images larger than the SRAM buffer (EasyFlash, Ocean,
 * C64 Game System, Magic Desk, GMod2).
 */

#ifndef WEACT_RP2350B_CORE_PSRAM_BOARD
#define WEACT_RP2350B_CORE_PSRAM_BOARD

// the SDK CMake reads the pico_board_cmake_set lines of this file only: they
// repeat the ones of weact_rp2350b_core.h
pico_board_cmake_set(PICO_PLATFORM, rp2350)
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)

// for board detection
#define WEACT_RP2350B_CORE_PSRAM

//------------- PSRAM ------------//
// QMI CS1 (XIP_CS1 is only on GPIO 0, 8, 19, 47). The PSRAM footprint on the
// bottom side has CS on GPIO 0 (C64 A0): it is rewired to GPIO 47, which is
// then no longer available for BUTTON2
#define GC_PSRAM_CS_PIN       47

#include "weact_rp2350b_core.h"

#endif
