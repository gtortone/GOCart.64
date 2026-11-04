#ifndef CONFIG_H
#define CONFIG_H

/*
 * Persistent launcher settings on the SD card
 *
 * The directory and file of the last launched cartridge/program are saved
 * in CONFIG_FILE, so the launcher starts there at the next power on.
 * Text format, one "key=value" per line:
 *   path=<current directory>
 *   file=<selected file>
 *   bus_edge=<cycles>    bus loop edge delay (c64_bus_edge_cycles)
 *   bus_hold=<cycles>    bus loop hold time (c64_bus_hold_cycles)
 * The bus timing lines are written only when the values differ from the
 * defaults (C64_BUS_EDGE_DELAY_CYCLES / C64_BUS_HOLD_CYCLES).
 */

// highest accepted bus timing value (cycles)
#define CONFIG_BUS_MAX   100

#define CONFIG_FILE  "/.gocart64.cfg"

// load cfg_file.path / cfg_file.file and the bus timing (call after the SD
// card is mounted)
bool config_load(void);

// save cfg_file.path / cfg_file.file and the bus timing (call after a
// successful launch, or after the bus timing has changed)
bool config_save(void);

#endif
