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
 */

#define CONFIG_FILE  "/.gocart64.cfg"

// load cfg_file.path / cfg_file.file (call after the SD card is mounted)
bool config_load(void);

// save cfg_file.path / cfg_file.file (call after a successful launch)
bool config_save(void);

#endif
