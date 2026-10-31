#include <stdio.h>      // printf
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

#include "cartridge.h"
#include "cartridge-pio.h"
#include "c64_interface.h"
#include "c64_bus.h"

#include "board.h"
#include "commands.h"

uint8_t crt_buf[CRT_BUFFER_SIZE] = {};
uint8_t crt_map[64] = {};
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

   multicore_reset_core1();

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

// mask of 8K bank number bits decoded by a cartridge of the given size
// (size rounded up to a power of two, like real ROM chips)
static uint32_t cart_bank_mask(uint32_t size) {
   uint32_t banks = (size + 8*1024 - 1) / (8*1024);
   uint32_t mask = 1;
   while (mask < banks)
      mask <<= 1;
   return mask - 1;
}

uint8_t run_cart(IDataReader &r) {

   multicore_reset_core1();

   // init cart memory
   memset(crt_buf, 0, sizeof(crt_buf));
   memset(crt_map, 0, sizeof(crt_map));

   // static: core1 reads it after this function has returned
   static core1_args_t args;

   CRTParser *crt = new CRTParser(r, crt_buf, sizeof(crt_buf), crt_map, sizeof(crt_map));
   if (crt->parse() != true) {
      printf("E: CRT parsing error\n");
      delete crt;
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

   uint8_t **banks;
   uint8_t *crt_map;
   uint8_t *rom_ptr;
   bool ultimax;
   uint8_t ram[256];

   CART_METHOD bool cpu_read(uint32_t control, uint8_t &data) {
      if (bus_rom_sel(control)) {
         data = rom_ptr[bus_addr(control) & 0x3FFF];
         return true;
      }
      if (!(control & IO2_MASK)) {
         data = ram[bus_addr(control) & 0xFF];
         return true;
      }
      return false;
   }

   CART_METHOD void commit(uint32_t control) {

      if (control & RW_MASK)
         return;

      uint32_t addr = bus_addr(control);
      uint8_t data = bus_data(control);

      if (!(control & IO1_MASK)) {

         switch (addr & 0xFF) {

            case 0x00:     // $DE00 bank register
               rom_ptr = banks[crt_map[data & 0x3F]];
               break;

            case 0x02: {   // $DE02 control register
               uint8_t mode = ef_mode_table[data & 0x07];
               c64_set_exrom_game(mode >> 1, mode & 0x01);
               ultimax = (mode == EF_MODE_ULTIMAX);
               break;
            }
         }

      } else {
         // $DF00-$DFFF RAM
         ram[addr & 0xFF] = data;
      }
   }

   CART_METHOD bool vic_enabled(void) { return ultimax; }

   CART_METHOD uint8_t vic_read(uint32_t addr) {
      return rom_ptr[(addr & 0x1FFF) | 0x2000];
   }
};

CART_FUNC void __time_critical_func(run_cart_easyflash)(void) {

   core1_args_t *p = (core1_args_t*) multicore_fifo_pop_blocking();

   // EasyFlash boots in Ultimax mode (set by run_cart from CRT header)
   EasyFlashCart cart = { p->crt_banks, p->crt_map, p->crt_banks[0], true, {} };

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
