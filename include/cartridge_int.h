#ifndef CARTRIDGE_INT_H
#define CARTRIDGE_INT_H

/*
 * cartridge.cpp internals shared with the bus diagnostics (bus_diag.cpp)
 */

#include <stdint.h>
#include "cartridge.h"

extern uint8_t crt_buf[CRT_BUFFER_SIZE];

// EF3 USB registers ($DE09 status, $DE0A data), shared between core0 and
// core1 (bus loop)
#define EF3_RX_RDY   0x80
#define EF3_TX_RDY   0x40
#define EF3_NOT_RDY  0x00
extern volatile uint8_t ef3_rx_rdy;     // data for the C64 available
extern volatile uint8_t ef3_tx_rdy;     // the C64 may send data
extern volatile uint8_t ef3_rx_data;
extern volatile uint8_t ef3_tx_data;

// EasyFlash bank register, published by core1 for EAPI commands on core0
extern volatile uint8_t ef_cur_bank;

// EasyFlash PSRAM cache: misses served by core1, timer (low 32 bits) at the last one
extern volatile uint32_t ef_misses;
extern volatile uint32_t ef_last_miss_us;

// EAPI commands served by cart_poll() from now on
void eapi_start(void);

// EasyFlash with crash detection (DEBUG builds, edge delay scan)
void run_cart_easyflash_scan(void);

#endif
