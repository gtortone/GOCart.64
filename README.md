# GOCart.64

Multicart for the Commodore 64 based on the Raspberry Pi RP2350.

GOCart.64 plugs into the C64 expansion port and emulates many cartridge types
in software: the RP2350 serves every bus cycle of the C64 in real time from
its SRAM. Files are selected from a menu on the C64 screen and loaded from an
SD card. The project started as a port of
[Kung Fu Flash](https://github.com/KimJorgensen/KungFuFlash) to the RP2350
and uses its C64 launcher.

## Features

- Menu on the C64 screen to browse the SD card and start files
- CRT cartridge images: 18 hardware types, including EasyFlash up to 1 MB
- Raw ROM images (8K, 16K, Ultimax)
- PRG and P00 programs
- Disk and tape images: browse D64/D71/D81 and T64, load the programs
- EasyFlash flash writes (EAPI) kept in a save file next to the CRT
- GMod2 EEPROM emulated and kept in a file next to the CRT
- Optional 8 MB QSPI PSRAM for cartridge images larger than the SRAM buffer
- Bus timing calibration cartridge: finds the timing that works on your C64
- Clean start: C64 RAM, VIC-II and SID cleared before every launch
- The menu starts from the directory of the last launched file
- Debug shell (USB and UART) with diagnostic tools
- I2C slave interface for an external controller

## Supported files

| Extension | Type | Action |
|---|---|---|
| `.crt` | Cartridge image | Run (see the supported types below) |
| `.rom`, `.bin` | Raw ROM image | Run: "CBM80" at `$8004` → 8K or 16K cartridge, otherwise Ultimax (4K/8K images mirrored). C128 cartridges are not supported |
| `.prg`, `.p00` | Program | Load and run, transferred by the launcher |
| `.d64`, `.d71`, `.d81` | Disk image | Browse, load a program |
| `.t64` | Tape image | Browse, load a program |

## Supported cartridge types

| CRT ID | Cartridge | Notes |
|---:|---|---|
| 0 | Normal cartridge | 8K, 16K, Ultimax |
| 4 | Simons' BASIC | |
| 5 | Ocean | up to 512K, PSRAM |
| 7 | Fun Play, Power Play | |
| 8 | Super Games | |
| 11 | Westermann Learning | |
| 12 | Rex Utility | |
| 13 | Final Cartridge I | |
| 15 | C64 Game System, System 3 | up to 512K, PSRAM |
| 16 | Warp Speed | |
| 17 | Dinamic | |
| 18 | Zaxxon, Super Zaxxon | |
| 19 | Magic Desk, Domark, HES Australia | up to 1 MB, PSRAM |
| 21 | Comal-80 | |
| 32 | EasyFlash | up to 1 MB, PSRAM, EAPI save file, EF3 USB registers |
| 43 | Prophet64 | |
| 57 | RGCD, Hucky | |
| 60 | GMod2 | up to 512K, PSRAM, EEPROM save file |

"PSRAM": images larger than the SRAM buffer (448K) are loaded into the PSRAM,
if the board has one (see [PSRAM](#psram)).

### EasyFlash

- Flash writes and erases through the EAPI are supported: the EAPI of the
  cartridge is replaced by the Kung Fu Flash EAPI driver, served by the
  RP2350.
- Changes are written to `<name>.sav` next to `<name>.crt`, one second after
  the last write and before another file is started. The CRT file is never
  modified: delete the `.sav` file to restore the original cartridge.
- The save file is checked against the CRT data (size and hash): a save file
  of another cartridge is ignored.
- The EF3 USB registers (`$DE09`, `$DE0A`) are emulated: the launcher uses
  them to load programs.

### GMod2

- 8K banks at `$8000`, up to 512K.
- The M93C86 serial EEPROM (2K, Microwire) is emulated, with all its
  commands (READ, WRITE, ERASE, EWEN, EWDS, ERAL, WRAL).
- The EEPROM is loaded from `<name>.eep` (2K, raw) when the cartridge starts
  and written one second after the last change.
- The flash write mode is not emulated: games that save to the flash
  instead of the EEPROM do not keep their data.

## PSRAM

Boards with the 8 MB QSPI PSRAM (APS6404L) can run cartridge images larger
than the SRAM buffer (448K): EasyFlash and Magic Desk up to 1 MB, Ocean,
C64 Game System and GMod2 up to 512K.

- The image is loaded into the PSRAM and checked after the load.
- The PSRAM is too slow to serve a bus cycle: the SRAM buffer becomes a cache
  of 8K chunks (55 chunks). Bank 0 is always kept in the cache.
- A bank register write that selects a chunk not in the cache stops the C64
  CPU with the DMA line while the chunk is copied (about 0.35-0.7 ms per 8K);
  the VIC-II keeps running.
- The PSRAM is detected at boot. Without it, the largest image is 448K.

## Bus timing calibration

The RP2350 samples the C64 bus after a short delay from every PHI2 edge
(**edge delay**) and keeps driving the data bus for a while after the end of
a phase (**hold time**). The right values depend on the C64 (board revision,
PLA type). The defaults (edge 10, hold 5 cycles of 4 ns) work on a C64C and
on a breadbin with a GAL PLA; the calibration cartridge finds the values for
your C64.

1. Build the calibration CRT:
   ```
   python3 utils/mkcart_buscal.py bus_calibration.crt
   ```
2. Copy it to the SD card and start it from the menu.
3. GOCart.64 restarts the C64 with a test program for every pair of the grid
   (edge 0-30, hold 0-20 cycles). The C64 screen shows the result map while
   the scan runs (a few minutes).
4. The chosen pair (the mean of the working pairs, checked again) is saved
   in the configuration file and shown on the screen.

The test program checks ROM reads in 16K and Ultimax mode (also with other
VIC-II banks and the display off), the IO2 RAM, code running from IO2, VIC-II
fetches from the cartridge in Ultimax mode and write bursts to I/O registers
(no spurious cartridge register writes). A pair fails on any error, or if
the C64 crashes.

The values can also be set from the debug shell (`bus edge <n> hold <n> save`).

## Configuration file

`/.gocart64.cfg` on the SD card, written by GOCart.64:

| Key | Meaning |
|---|---|
| `path=`, `file=` | Directory and file of the last launched file (the menu starts there) |
| `bus_edge=` | Edge delay in cycles (only if not the default) |
| `bus_hold=` | Hold time in cycles (only if not the default) |

## Hardware

GOCart.64 runs on a WeAct Studio RP2350B Core board (RP2350B, 16 MB flash).
Two board definitions are available in `boards/`:

| Board | Description |
|---|---|
| `weact_rp2350b_core` | Without PSRAM, GPIO 47 = button 2 |
| `weact_rp2350b_core_psram` | 8 MB PSRAM. Its footprint has the chip select on GPIO 0 (C64 A0): it is rewired to GPIO 47 (QMI CS1), which is then not available for button 2 |

### Pin assignment

| Function | GPIO |
|---|---|
| C64 A0-A15 | 0-15 |
| R/W | 16 |
| ROML, ROMH | 17, 18 |
| IO1, IO2 | 19, 20 |
| PHI2 | 21 |
| BA | 22 |
| C64 D0-D7 | 23-30 |
| NMI, IRQ, RESET | 31, 32, 33 |
| DMA | 38 |
| DOT clock | 39 |
| GAME, EXROM | 44, 45 |
| SD card (SPI0): SCK, TX, RX, CS | 34, 35, 36, 37 |
| EXT header (SPI1): RX, CS, SCK, TX | 40, 41, 42, 43 |
| I2C0 slave: SDA, SCL | 40, 41 |
| Debug UART1: TX, RX | 42, 43 |
| Button 1 | 46 |
| Button 2 / PSRAM CS | 47 |

## Building

Requirements:

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) (path set in
  `CMakeLists.txt`: `/usr/share/pico-sdk`)
- ARM GCC toolchain (`arm-none-eabi-gcc`), CMake
- [cc65](https://cc65.github.io/) for the C64 launcher
- `uf2conv` to build the UF2 image
- Python 3 for the utilities

Build the launcher first, then the firmware:

```
cd launcher
make
cd ..

# board without PSRAM
cmake -B build/release -DPICO_BOARD=weact_rp2350b_core
cmake -B build/debug -DPICO_BOARD=weact_rp2350b_core -DDEBUG=ON

# board with PSRAM
cmake -B build/release -DPICO_BOARD=weact_rp2350b_core_psram
cmake -B build/debug -DPICO_BOARD=weact_rp2350b_core_psram -DDEBUG=ON

make -C build/release
```

The sources are collected with `file(GLOB)`: run `cmake` again after adding
a source file.

Outputs in the build directory:

| File | Content |
|---|---|
| `GOCart.64.img.uf2` | Firmware and launcher, to copy to the board in BOOTSEL mode |
| `GOCart.64.img.bin` | Firmware and launcher, to program at `0x10000000` (e.g. with OpenOCD) |

With OpenOCD and a CMSIS-DAP probe:

```
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 20000"
program build/debug/GOCart.64.img.bin 0x10000000 verify reset
```

## Debug shell

Debug builds (`-DDEBUG=ON`) have a command shell on the USB serial port and
on UART1 (GPIO 42 TX, 43 RX).

| Command | Description |
|---|---|
| `ls`, `lss`, `cd <dir>` | List the SD card (`lss`: sorted), change directory |
| `load <file>` | Start a file (CRT, PRG, ROM...) |
| `menu` | Back to the launcher menu |
| `reset c64` / `reset pico` | Reset the C64 or the RP2350 |
| `test [start] [pattern] [noclean]` | Start every supported file of the current directory in turn, results in a log file |
| `clean [on \| off]` | Clean start before every launch, or run it now |
| `bus [edge <n>] [hold <n>] [default] [save]` | Bus timing values |
| `bus scan <file.crt> [seconds [from to [step [repeat]]]]` | Start an EasyFlash CRT with every edge delay value; crashes (JAM, BRK) are detected |
| `psram [test [kb]]` | PSRAM information and memory test |
| `cputrace ...` | EasyFlash CPU trace (`EF_DEBUG_CPU_TRACE` in `include/bus_diag.h`): every bus cycle recorded, with triggers on addresses, JAM and BRK |
| `info` | Memory usage |

## I2C interface

GOCart.64 is an I2C slave (address `0x50`, 400 kHz, GPIO 40/41) for an
external controller. Frames start with `0xAA` and end with a CRC-16 (CCITT).
Commands: ping, read/write register, read/write cartridge buffer location,
write cartridge buffer, set LED.

## Utilities

| Script | Description |
|---|---|
| `utils/mkcart_buscal.py` | Bus timing calibration CRT |
| `utils/mkcart_banktest.py` | Bank test CRTs for Ocean, C64 Game System, Magic Desk, GMod2 (512K-1 MB, for the PSRAM) |
| `utils/mkcart_readtest.py`, `utils/mkcart_copytest.py` | EasyFlash ROM read test CRTs |
| `utils/mkcart_streamtest.py` | EF3 USB transfer test CRT (with the VIC-II stealing the bus) |
| `utils/crt_scan.py` | List the hardware type of every CRT file in a directory tree |
| `utils/ef_repack.py` | Split large EasyLoader EasyFlash compilations into smaller CRT files |

## Source layout

| Path | Content |
|---|---|
| `src/cartridge.cpp` | Cartridge handlers (one bus loop per type, on core1), `run_cart()` |
| `include/c64_bus.h` | Generic bus loop, bus timing |
| `src/ef_cache.cpp` | Chunk tables and SRAM/PSRAM cache |
| `src/ef_save.cpp` | EasyFlash save files, GMod2 EEPROM files |
| `src/bus_diag.cpp` | Bus calibration, bus scan, CPU trace |
| `include/CRTParser.h` | CRT parser |
| `src/psram.cpp` | PSRAM detection and setup |
| `src/menu*.cpp` | Menu, SD card, D64 and T64 browsing |
| `src/shell.cpp` | Debug shell |
| `launcher/` | C64 launcher (cc65), from Kung Fu Flash |
| `boards/` | Board definitions |

## Credits

- [Kung Fu Flash](https://github.com/KimJorgensen/KungFuFlash) by Kim
  Jørgensen: launcher, menu and EAPI driver
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
  by Carl Kugler: SD card and FAT file system

## License

MIT License, see [LICENSE](LICENSE). Code taken from other projects keeps
its own license.
