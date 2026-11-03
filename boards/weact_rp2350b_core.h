#ifndef WEACT_RP2350B_CORE_BOARD
#define WEACT_RP2350B_CORE_BOARD

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// for board detection
#define WEACT_RP2350B_CORE

#define RASPBERRYPI_PICO2

#define PICO_RP2350A 0     // 1 for RP2350A, 0 for RP2350B

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

//------------- SPI0 -------------//
#define GC_SD_SPI             0
#define GC_SD_SPI_RX_PIN      36
#define GC_SD_SPI_CSN_PIN     37
#define GC_SD_SPI_SCK_PIN     34
#define GC_SD_SPI_TX_PIN      35
 
//------------- SPI1 -------------//
#define GC_EXT_SPI            1
#define GC_EXT_SPI_RX_PIN     40
#define GC_EXT_SPI_CSN_PIN    41
#define GC_EXT_SPI_SCK_PIN    42
#define GC_EXT_SPI_TX_PIN     43

//------------- I2C0 -------------//
#define GC_EXT_I2C            0
#define GC_EXT_I2C_SCL_PIN    41
#define GC_EXT_I2C_SDA_PIN    40

//------ cartridge interface -----//
#define GC_ADDR0_PIN          0
#define GC_RW_PIN             16
#define GC_ROML_PIN           17
#define GC_ROMH_PIN           18
#define GC_IO1_PIN            19
#define GC_IO2_PIN            20
#define GC_PHI2_PIN           21
#define GC_BA_PIN             22
#define GC_DATA0_PIN          23
#define GC_NMI_PIN            31
#define GC_IRQ_PIN            32
#define GC_RESET_PIN          33 
#define GC_DMA_PIN            38
#define GC_DOTCLK_PIN         39
#define GC_GAME_PIN           44
#define GC_EXROM_PIN          45

//------------- PSRAM ------------//
// QMI CS1 (XIP_CS1 is only on GPIO 0, 8, 19, 47). The PSRAM footprint on the
// bottom side has CS on GPIO 0 (C64 A0): it is rewired to GPIO 47, which is
// no longer used by BUTTON2
#define GC_PSRAM_CS_PIN       47

//------------ buttons ----------//
#define GC_BUTTON1_PIN        46
// GC_BUTTON2_PIN: GPIO 47 is the PSRAM chip select

//------------ debug ------------//
// UART1 on the free pins of the EXT SPI header (GPIO 40/41 are used by the
// I2C slave): GPIO 42/43 have UART1 TX/RX as auxiliary function (RP2350)
#define GC_DBG_UART_ID        1
#define GC_DBG_UART_TX_PIN    42
#define GC_DBG_UART_RX_PIN    43

#endif

