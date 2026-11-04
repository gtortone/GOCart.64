/*
 * Copyright (c) 2019-2024 Kim Jørgensen
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************
 * Derived from Draco Browser V1.0C 8 Bit (27.12.2009)
 * Created 2009 by Sascha Bader
 *
 * The code can be used freely as long as you retain
 * a notice describing original source and author.
 *
 * THE PROGRAMS ARE DISTRIBUTED IN THE HOPE THAT THEY WILL BE USEFUL,
 * BUT WITHOUT ANY WARRANTY. USE THEM AT YOUR OWN RISK!
 *
 * Newer versions might be available here: http://www.sascha-bader.de/html/code.html
 */

#ifndef SCREEN_H
#define SCREEN_H

#include <conio.h>
#include <stdint.h>

#define SCREEN_RAM ((unsigned char *)0x0400)

#define SCREENH 25
#define BOTTOM (SCREENH-1)
#define SCREENW 40
#define DIRH (SCREENH-2)

/*
 * Color themes, LAUNCHER_THEME (CMake: -DGOCART_LAUNCHER_THEME=n):
 *   0  white on blue, light blue bars
 *   1  light gray on black, cyan selection, light blue bars (default)
 *   2  light green on black, white selection, green bars
 *   3  yellow on black, white selection, orange bars
 * The selected entry: characters in SELTXTC on a bar in SELC (the bar is
 * the background color of its row, changed by a raster interrupt, see
 * selbar_init in launcher_asm.s). Title and status bars are shown in
 * reverse: characters in the background color on BACKC.
 */
/*
 * LAUNCHER_THIN_FONT (CMake: -DGOCART_LAUNCHER_THIN_FONT=ON/OFF): 1 = font
 * with 1 pixel wide strokes made from the character ROM (see screen.c),
 * 0 = character ROM font
 */
#ifndef LAUNCHER_THIN_FONT
#define LAUNCHER_THIN_FONT 1
#endif

#ifndef LAUNCHER_THEME
#define LAUNCHER_THEME 1
#endif

#if LAUNCHER_THEME == 0
#define BORDERC COLOR_BLUE          // border
#define BGC COLOR_BLUE              // background
#define TEXTC COLOR_WHITE           // file names, help text
#define SELC COLOR_WHITE            // selection bar, search input
#define SELTXTC COLOR_BLUE          // selected file
#define BACKC COLOR_LIGHTBLUE       // title and status bars, frame
#define SEARCHC COLOR_CYAN          // progress bar
#define WARNC COLOR_YELLOW          // text elements, warnings
#define ERRORC COLOR_LIGHTRED       // errors
#elif LAUNCHER_THEME == 2
#define BORDERC COLOR_BLACK
#define BGC COLOR_BLACK
#define TEXTC COLOR_LIGHTGREEN
#define SELC COLOR_WHITE
#define SELTXTC COLOR_BLACK
#define BACKC COLOR_GREEN
#define SEARCHC COLOR_GRAY2
#define WARNC COLOR_YELLOW
#define ERRORC COLOR_LIGHTRED
#elif LAUNCHER_THEME == 3
#define BORDERC COLOR_BLACK
#define BGC COLOR_BLACK
#define TEXTC COLOR_YELLOW
#define SELC COLOR_WHITE
#define SELTXTC COLOR_BLACK
#define BACKC COLOR_ORANGE
#define SEARCHC COLOR_GRAY2
#define WARNC COLOR_LIGHTRED
#define ERRORC COLOR_LIGHTRED
#else
#define BORDERC COLOR_BLACK
#define BGC COLOR_BLACK
#define TEXTC COLOR_GRAY2
#define SELC COLOR_CYAN
#define SELTXTC COLOR_BLACK
#define BACKC COLOR_LIGHTBLUE
#define SEARCHC COLOR_GRAY2
#define WARNC COLOR_YELLOW
#define ERRORC COLOR_LIGHTRED
#endif

void clearArea(int xpos, int ypos, int xsize, int ysize);
void drawFrame(void);
void initScreen(unsigned char border, unsigned char bg, unsigned char text);
#endif
