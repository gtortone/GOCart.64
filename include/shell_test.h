#ifndef SHELL_TEST_H_
#define SHELL_TEST_H_

/*
 * Shell "test" command: launches every supported file of the current
 * directory in alphabetical order (CRT, ROM/BIN, PRG/P00, first PRG of
 * T64/D64) and asks on the console whether it started.
 * Results are printed and appended to TEST_LOG_FILE in the same directory.
 *
 * pattern: optional file name pattern (e.g. "*.crt"), NULL or "" for all files
 * start:   index of the first file to launch (1 = first, 0 = first), to
 *          resume an interrupted test; the numbering is the same as a full run
 * clean:   bring the C64 to a power-on like state before every title
 *          (RAM, VIC-II, SID cleared, see run_c64_clean)
 */

#define TEST_LOG_FILE  "gocart_test.log"

#include <cstdint>

void shell_test(const char *pattern, uint32_t start, bool clean);

#endif
