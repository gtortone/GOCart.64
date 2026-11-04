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

#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <c64.h>
#include "screen.h"

void clearArea(int xpos, int ypos, int xsize, int ysize)
{
  int y = ypos;
  for (y=ypos;y<(ypos+ysize);++y)
  {
	  cclearxy(xpos,y,xsize);
  }
}

void drawFrame(void)
{
  uint8_t i;

  for (i=1; i<SCREENH-1; i++)
  {
    // left border
    cputcxy(0, i, '\xa5');

    // right border
    cputcxy(SCREENW-1, i, '\xa7');
  }
}

#if LAUNCHER_THIN_FONT
/*
 * Thin font: the upper/lower case character set of the C64 character ROM
 * copied to FONT_RAM with the 2 pixel wide strokes made 1 pixel wide: the
 * rightmost pixel of every horizontal run of 2 or more pixels is removed
 * (also horizontal bars get 1 pixel shorter on the right, so they still end
 * at the thinned vertical strokes); full rows are kept (block graphics).
 * Characters 128-255 are the reverse of 0-127, as in the ROM. The VIC-II
 * reads it from RAM at $2000 (bank 0).
 * Starting a program or a cartridge resets the VIC-II (KERNAL CINT): the
 * ROM font is back before the RAM at $2000 is overwritten.
 */
#define FONT_RAM   ((uint8_t *)0x2000)
#define CHAR_ROM   ((const uint8_t *)0xd800)     // upper/lower case set

static void initFont(void)
{
  uint16_t i;
  uint8_t port, b, m;

  // character ROM at $D000 (CHAREN low), interrupts off meanwhile
  __asm__("sei");
  port = *(uint8_t *)1;
  *(uint8_t *)1 = port & ~0x04;
  for (i=0; i<1024; i++)
  {
    FONT_RAM[i] = CHAR_ROM[i];
  }
  *(uint8_t *)1 = port;
  __asm__("cli");

  for (i=0; i<1024; i++)
  {
    b = FONT_RAM[i];
    if (b != 0xff)
    {
      // pixel k (bit 7 = left) set, left neighbour set, right neighbour
      // clear: rightmost pixel of a run
      m = b & (b >> 1) & ~(b << 1);
      b &= ~m;
    }
    FONT_RAM[i] = b;
    FONT_RAM[1024 + i] = ~b;
  }

  // screen $0400, characters $2000
  VIC.addr = (VIC.addr & 0xf0) | 0x08;
}
#endif

/* initialize screen mode */
void initScreen(unsigned char border, unsigned char bg, unsigned char text)
{
#if LAUNCHER_THIN_FONT
  initFont();
#endif
  bordercolor(border);
  bgcolor(bg);
  textcolor(text);
  clrscr();
}
