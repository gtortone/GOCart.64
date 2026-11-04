#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include "board.h"
#include "psram.h"

static size_t psram_bytes;

#ifdef GC_PSRAM_CS_PIN

// APS6404L: max 133 MHz (QPI), CS low max 8 us (tCEM), CS high min 18 ns
#define PSRAM_MAX_FREQ_HZ     133000000
#define PSRAM_TCEM_FS         (8000000000ll / 64)   // max select, 64 clock units
#define PSRAM_TCPH_FS         18000000ll

//
// direct mode transfers: code in RAM, interrupts off (the flash cannot be
// read while the QMI is in direct mode)
//

static void __no_inline_not_in_flash_func(qmi_direct_wait)(void) {
   while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
      ;
}

static uint8_t __no_inline_not_in_flash_func(qmi_direct_xfer)(uint32_t tx) {
   qmi_hw->direct_tx = tx;
   while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS))
      ;
   qmi_direct_wait();
   return (uint8_t)qmi_hw->direct_rx;
}

// one command byte, CS asserted only for it (quad: QPI mode, else SPI)
static void __no_inline_not_in_flash_func(qmi_direct_cmd)(uint8_t cmd, bool quad) {
   qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
   if (quad)
      qmi_direct_xfer(QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | cmd);
   else
      qmi_direct_xfer(cmd);
   qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

// busy wait in RAM (the flash is not accessible in direct mode)
static void __no_inline_not_in_flash_func(ram_delay)(uint32_t loops) {
   while (loops--)
      __asm volatile("nop");
}

// reset, read ID and enter QPI mode; returns MFID << 16 | KGD << 8 | EID,
// 0 if no answer
static uint32_t __no_inline_not_in_flash_func(psram_detect)(void) {

   uint32_t irq = save_and_disable_interrupts();

   qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
   qmi_direct_wait();

   // exit QPI mode, if the chip is already in it (quad command 0xF5)
   qmi_direct_cmd(0xF5, true);

   // software reset (reset enable 0x66 + reset 0x99, SPI): the chip may be in
   // an unknown state if CS was low at power-up (RP2350 pads have a pull-down)
   qmi_direct_cmd(0x66, false);
   qmi_direct_cmd(0x99, false);
   ram_delay(25000);          // > 50 us at 250 MHz

   // read ID (SPI): 0x9F, 3 address bytes, MFID, KGD, EID
   uint8_t mfid = 0, kgd = 0, eid = 0;
   qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
   for (int i = 0; i < 7; i++) {
      uint8_t rx = qmi_direct_xfer(i == 0 ? 0x9F : 0xFF);
      if (i == 4) mfid = rx;
      else if (i == 5) kgd = rx;
      else if (i == 6) eid = rx;
   }
   qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

   uint32_t id = 0;
   if (kgd == 0x5D) {
      id = (uint32_t)mfid << 16 | (uint32_t)kgd << 8 | eid;
      // enter QPI mode (SPI command 0x35)
      qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
      qmi_direct_xfer(0x35);
      qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
   }

   qmi_hw->direct_csr = 0;
   restore_interrupts(irq);
   return id;
}

// QSPI timing, QPI read 0xEB (6 wait cycles) and write 0x38
static void __no_inline_not_in_flash_func(psram_configure)(uint32_t divisor) {

   uint32_t sys_hz = clock_get_hz(clk_sys);
   uint32_t rxdelay = divisor;
   if (sys_hz / divisor > 100000000)
      rxdelay++;

   int64_t period_fs = 1000000000000000ll / sys_hz;
   uint32_t max_select = PSRAM_TCEM_FS / period_fs;
   int32_t min_deselect = (PSRAM_TCPH_FS + period_fs - 1) / period_fs - (divisor + 1) / 2;
   if (min_deselect < 0)
      min_deselect = 0;

   uint32_t irq = save_and_disable_interrupts();

   qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                         QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                         max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                         (uint32_t)min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                         rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                         divisor << QMI_M1_TIMING_CLKDIV_LSB;
   qmi_hw->m[1].rfmt = QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
                       QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                       QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                       QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                       QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
                       QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
                       QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB;
   qmi_hw->m[1].rcmd = 0xEB << QMI_M1_RCMD_PREFIX_LSB;
   qmi_hw->m[1].wfmt = QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
                       QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
                       QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
                       QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
                       QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
                       QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;
   qmi_hw->m[1].wcmd = 0x38 << QMI_M1_WCMD_PREFIX_LSB;

   xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

   restore_interrupts(irq);
}

size_t psram_init(void) {

   if (psram_bytes)
      return psram_bytes;

   // CS high (idle) for at least 150 us before the first command (APS6404L
   // power-up); a 10K pull-up on CS keeps it high from power-on
   gpio_set_function(GC_PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
   sleep_us(200);

   uint32_t id = psram_detect();
   if (!id) {
      printf("E: PSRAM not found (CS GPIO %d)\n", GC_PSRAM_CS_PIN);
      return 0;
   }

   // size from the EID (APS6404L: 8 MB)
   uint8_t eid = id & 0xFF;
   switch (eid >> 5) {
      case 0:  psram_bytes = 2u << 20; break;
      case 1:  psram_bytes = 4u << 20; break;
      default: psram_bytes = 8u << 20; break;
   }
   if (eid == 0x26)
      psram_bytes = 8u << 20;

   // QSPI clock: sys / 2 (<= 133 MHz)
   uint32_t sys_hz = clock_get_hz(clk_sys);
   uint32_t divisor = (sys_hz + PSRAM_MAX_FREQ_HZ - 1) / PSRAM_MAX_FREQ_HZ;
   if (divisor < 2)
      divisor = 2;
   psram_configure(divisor);

   printf("PSRAM: %u MB, MFID $%02lX KGD $%02lX EID $%02lX, QSPI %lu MHz\n",
          (unsigned)(psram_bytes >> 20), (unsigned long)(id >> 16),
          (unsigned long)((id >> 8) & 0xFF), (unsigned long)eid,
          (unsigned long)(sys_hz / divisor / 1000000));
   return psram_bytes;
}

#else

size_t psram_init(void) {
   return 0;
}

#endif

size_t psram_size(void) {
   return psram_bytes;
}

//
// shell
//

static uint32_t test_value(uint32_t ofs, uint32_t seed) {
   uint32_t x = ofs * 2654435761u + seed;
   return x ^ (x >> 13);
}

// written and read back through the uncached alias, two patterns
static bool psram_test(uint32_t bytes) {

   volatile uint32_t *mem = (volatile uint32_t *)PSRAM_UNCACHED_BASE;
   uint32_t words = bytes / 4;
   uint32_t errors = 0;

   for (int pass = 0; pass < 2; pass++) {
      uint32_t seed = pass ? 0x5A5A1234u : 0x13572468u;
      uint64_t t0 = time_us_64();
      for (uint32_t i = 0; i < words; i++)
         mem[i] = test_value(i, seed);
      uint64_t t1 = time_us_64();
      for (uint32_t i = 0; i < words; i++) {
         uint32_t v = mem[i];
         if (v != test_value(i, seed) && ++errors <= 8)
            printf("  $%06lX: $%08lX != $%08lX\n", (unsigned long)(i * 4),
                   (unsigned long)v, (unsigned long)test_value(i, seed));
      }
      uint64_t t2 = time_us_64();
      printf("  pass %d: %lu KB, write %lu KB/s, read %lu KB/s\n", pass + 1,
             (unsigned long)(bytes >> 10),
             (unsigned long)((uint64_t)bytes * 1000 / (t1 - t0 + 1)),
             (unsigned long)((uint64_t)bytes * 1000 / (t2 - t1 + 1)));
   }

   printf("PSRAM test %s (%lu errors)\n", errors ? "FAILED" : "OK", (unsigned long)errors);
   return errors == 0;
}

void psram_shell(char *args) {

   char *token = args ? strtok(args, " ") : NULL;

   if (!psram_bytes) {
#ifdef GC_PSRAM_CS_PIN
      printf("psram: not found (CS GPIO %d)\n", GC_PSRAM_CS_PIN);
#else
      printf("psram: GC_PSRAM_CS_PIN not set in the board header\n");
#endif
      return;
   }

   if (token && strcmp(token, "test") == 0) {
      token = strtok(NULL, " ");
      uint32_t kb = token ? strtoul(token, NULL, 10) : 1024;
      if (kb == 0 || kb * 1024 > psram_bytes)
         kb = psram_bytes >> 10;
      printf("psram: test of %lu KB (the PSRAM contents are lost)\n", (unsigned long)kb);
      psram_test(kb * 1024);
      return;
   }

   printf("psram: %u MB at $%08lX (uncached)\n", (unsigned)(psram_bytes >> 20),
          (unsigned long)PSRAM_UNCACHED_BASE);
}
