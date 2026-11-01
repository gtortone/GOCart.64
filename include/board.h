#ifndef BOARD_H_
#define BOARD_H_

#include "hardware/structs/sio.h"
#include <hardware/i2c.h>

#define PINROMADDR   GC_ADDR0_PIN
#define ADDRWIDTH    16

// CONTROL pins
#define RW           GC_RW_PIN
#define ROML         GC_ROML_PIN
#define ROMH         GC_ROMH_PIN
#define IO1          GC_IO1_PIN
#define IO2          GC_IO2_PIN
#define PHI2         GC_PHI2_PIN
#define BA           GC_BA_PIN

#define PINROMDATA   GC_DATA0_PIN
#define DATAWIDTH    8

#define NMI          GC_NMI_PIN
#define IRQ          GC_IRQ_PIN
#define RESET        GC_RESET_PIN
#define DMA          GC_DMA_PIN
#define DOTCLK       GC_DOTCLK_PIN
#define GAME         GC_GAME_PIN
#define EXROM        GC_EXROM_PIN

// SD pins
#define SD_SPI_PORT  spi_get_instance(GC_SD_SPI)
#define SD_SCK       GC_SD_SPI_SCK_PIN
#define SD_MOSI      GC_SD_SPI_TX_PIN
#define SD_MISO      GC_SD_SPI_RX_PIN
#define SD_CS        GC_SD_SPI_CSN_PIN
#define SD_SPEED     15 * 1000 * 1000

// I2C pins
#define I2C_PORT     i2c_get_instance(GC_EXT_I2C)
#define I2C_ADDR     0x50
#define I2C_SDA      GC_EXT_I2C_SDA_PIN 
#define I2C_SCL      GC_EXT_I2C_SCL_PIN
#define I2C_BAUDRATE 400000

// SPI pins
#define SPI_PORT     spi_get_instance(GC_EXT_SPI)
#define SPI_MISO     GC_EXT_SPI_RX_PIN
#define SPI_CS       GC_EXT_SPI_CSN_PIN
#define SPI_CLK      GC_EXT_SPI_SCK_PIN
#define SPI_MOSI     GC_EXT_SPI_TX_PIN

#ifdef DEBUG
   #define UART_ID   uart_get_instance(GC_DBG_UART_ID)
   #define UART_TX   GC_DBG_UART_TX_PIN 
   #define UART_RX   GC_DBG_UART_RX_PIN 
   #define UART_BAUDRATE 115200
#endif

#define RW_MASK      ((uint32_t)1 << RW)
#define ROML_MASK    ((uint32_t)1 << ROML)
#define ROMH_MASK    ((uint32_t)1 << ROMH)
#define IO1_MASK     ((uint32_t)1 << IO1)
#define IO2_MASK     ((uint32_t)1 << IO2)
#define PHI2_MASK    ((uint32_t)1 << PHI2)
#define BA_MASK      ((uint32_t)1 << BA)

// masks
#define ADDR_GPIO_MASK     (0xFFFF << PINROMADDR)
#define DATA_GPIO_MASK     (0xFF << PINROMDATA)

#define SET_DATA_MODE_OUT     sio_hw->gpio_oe_set = DATA_GPIO_MASK;
#define SET_DATA_MODE_IN      sio_hw->gpio_oe_clr = DATA_GPIO_MASK;

#define GPIO_GET_LOW_32(v)    pico_default_asm_volatile ("mrc p0, #0, %0, c0, c8" : "=r" (v));
#define GPIO_GET_HIGH_32(v)   pico_default_asm_volatile ("mrc p0, #0, %0, c0, c9" : "=r" (v));

#define ADDR_IN (sio_hw->gpio_in & ADDR_GPIO_MASK) >> PINROMADDR
#define DATA_OUT(v) sio_hw->gpio_out = (sio_hw->gpio_out & ~(0xFF << PINROMDATA)) | ((v)<<PINROMDATA);
#define DATA_IN ((sio_hw->gpio_in & DATA_GPIO_MASK) >> PINROMDATA)
#define DATA_IN_BYTE DATA_IN

#define FLASH_AREA_SIZE    1024 * 1024     // 1 MB
#define FLASH_AREA_OFFSET  PICO_FLASH_SIZE_BYTES - FLASH_AREA_SIZE

#define I2C_MAX_PAYLOAD 1000
#define I2C_RX_BUF_SIZE (I2C_MAX_PAYLOAD * 2)
#define I2C_TX_BUF_SIZE (I2C_MAX_PAYLOAD * 2)

#define SYST_CSR (*(volatile uint32_t*)0xE000E010)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018)

void board_setup(void);

#endif
