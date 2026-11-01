#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include "board.h"
#include "FileReader.h"
#include "BufferReader.h"
#include "Reader.h"
#include "CRTParser.h"

#define BANKS_NUM          30
#define CRT_BUFFER_SIZE    (BANKS_NUM * 16 * 1024)

extern uint8_t crt_buf[CRT_BUFFER_SIZE];

#define CRT_BANK(bank)     (crt_buf + (uint32_t)(16 * 1024 * bank))

extern uint8_t __flash_binary_end[];
#define CRT_LAUNCHER       ((uint8_t *)__flash_binary_end)
#define CRT_LAUNCHER_SIZE  16384

// KFF data stream buffer (64K, 16-bit pointers): must not overlap the
// launcher ROM copy in CRT_BANK(0), it is needed again to load PRG files
#define KFF_BUF   (CRT_BANK(8))

// PRG file buffer (64K, load address + data)
#define PRG_BUF        (CRT_BANK(12))
#define PRG_BUF_SIZE   (64 * 1024)

// $de01 Command register in KFF RAM
#define KFF_COMMAND (*((volatile uint8_t*)(kff_ram + 1)))

// $de04-$de05 Read buffer pointer register in KFF RAM
#define KFF_READ_PTR (*((uint16_t*)(kff_ram + 4)))

// $de06-$de07 Write buffer pointer register in KFF RAM
#define KFF_WRITE_PTR (*((uint16_t*)(kff_ram + 6)))

// $de09 ID register in KFF RAM (same address as EF3 USB Control register)
#define KFF_ID (*((uint8_t*)(kff_ram + 9)))

#define KFF_ID_VALUE 0x2A

typedef struct {
    uint8_t *crt_buf;
    uint8_t **crt_banks;
    uint8_t *crt_map;
    bool ultimax;           // cartridge starts in Ultimax mode (CRT header)
    uint32_t bank_mask;     // 8K bank number bits decoded by the cartridge
} core1_args_t;

#define COMPILER_BARRIER() asm volatile("" ::: "memory")

void launcher_enable(void);
void launcher_disable(void);
bool launcher_running(void);

// file_name: CRT file (relative to the current directory or absolute), used
// for the EasyFlash save file "<name>.sav"; NULL: changes are not saved
uint8_t run_cart(IDataReader &r, const char *file_name = NULL);
uint8_t run_launcher(void);
bool run_prg(const uint8_t *prg, uint32_t size, const char *name);

// raw cartridge image (.ROM/.BIN)
#define ROM_OK            0
#define ROM_UNSUPPORTED   1
#define ROM_C128          2
uint8_t run_rom(uint8_t *rom, uint32_t len);

// bring the C64 to a power-on like state (RAM, VIC-II, SID cleared),
// the C64 is then kept in reset until the next cartridge is started
bool run_c64_clean(void);

// clean start before every cartridge/program (run_cart, run_rom, run_prg),
// enabled by default
void c64_clean_enable(bool enable);
bool c64_clean_is_enabled(void);

// serve requests from the running cartridge (EasyFlash EAPI), call from core0 idle loop
void cart_poll(void);

void run_cart_normal(void);
void run_cart_magic_desk(void);
void run_cart_ocean(void);
void run_cart_fun_play(void);
void run_cart_super_games(void);
void run_cart_easyflash(void);
void run_cart_dinamic(void);
void run_cart_zaxxon(void);
void run_cart_kff(void);
void run_cart_ef3(void);

void kff_set_command(uint8_t cmd);
bool kff_get_reply(uint8_t cmd, uint8_t *reply);
uint8_t kff_receive_byte(void);
void kff_send_byte(uint8_t data);
void kff_init(void);
void kff_dump(void);

#endif
