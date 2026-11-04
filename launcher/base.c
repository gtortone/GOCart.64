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

#include <stdint.h>
#include <stdio.h>
#include <conio.h>
#include <joystick.h>
#include <c64.h>
#include <stdbool.h>
#include "base.h"
#include "screen.h"
#include "launcher_asm.h"

#define LONG_PRESS  (180)
#define KEY_PRESS   (30)
#define KEY_REPEAT  (10)
#define MIN_PRESS   (4)
#define DEBOUNCE    (30)

/*
 * Joystick in port 1 or port 2. Port 1 shares the lines of the keyboard
 * matrix: it is read with no keyboard column selected (keys pressed do not
 * show up as joystick moves), and the keys it makes the KERNAL see while it
 * is moved (and JOY1_HOLDOFF jiffies after) are discarded (keyHit).
 */
#define JOY1_HOLDOFF (6)                        // jiffies (1/60 s)
#define JIFFY_LOW    (*(volatile uint8_t *)0xa2)
#define KEY_COUNT    (*(volatile uint8_t *)0xc6) // keys in the KERNAL buffer

static bool joy1Recent = false;
static uint8_t joy1Jiffy;

// joysticks in both ports; activity in port 1 starts the JOY1_HOLDOFF time
// in which the keys seen by the KERNAL are discarded (keyHit), also when it
// is seen only by getJoy()
static void joy1Seen(void)
{
    if (joy1_bits)
    {
        joy1Recent = true;
        joy1Jiffy = JIFFY_LOW;
    }
}

static uint8_t debounceJoy(void)
{
    uint8_t i, pressed = 0;

    joy1_bits = 0;
    for (i=0; i<DEBOUNCE; i++)
    {
        pressed |= read_joys();
    }
    joy1Seen();

    return pressed;
}

bool keyHit(void)
{
    joy1_bits = 0;
    read_joys();
    joy1Seen();

    if (joy1Recent)
    {
        KEY_COUNT = 0;                          // keys made by joystick 1
        if ((uint8_t)(JIFFY_LOW - joy1Jiffy) < JOY1_HOLDOFF)
        {
            return false;
        }
        joy1Recent = false;
        return false;
    }

    return kbhit();
}

uint8_t getJoy(void)
{
    static uint8_t last;
    uint8_t j, btn_last = 0;
    uint16_t cnt, cnt_max = KEY_PRESS;

    if (last)
    {
        if (JOY_BTN_1(last))
        {
            btn_last = last;
        }
        else
        {
            cnt_max = KEY_REPEAT;
        }
    }

    for (cnt=0; cnt<cnt_max; cnt++)
    {
        last = debounceJoy();
        if (!last)
        {
            break;
        }

        if (JOY_BTN_1(last))
        {
            if (last == JOY_BTN_1_MASK) // only button
            {
                if (btn_last)
                {
                    break;
                }

                cnt_max = LONG_PRESS;
            }
            else    // button and direction
            {
                if (btn_last == last)
                {
                    break;
                }

                btn_last = JOY_BTN_1_MASK;
                cnt_max = KEY_PRESS;
            }
        }

        j = last;
    }

    if (cnt < MIN_PRESS)
    {
        return CH_NONE;
    }

    if (JOY_BTN_1(j))
    {
        if (j != JOY_BTN_1_MASK)
        {
            return JOY_UP(j) ? CH_FIRE_UP :
                    JOY_DOWN(j) ? CH_FIRE_DOWN :
                     JOY_LEFT(j) ? CH_FIRE_LEFT : CH_FIRE_RIGHT;
        }

        if (cnt < LONG_PRESS)
        {
            return CH_ENTER;    // short press
        }

        return CH_SHIFT_ENTER;  // long press
    }

    return JOY_UP(j) ? CH_CURS_UP :
            JOY_DOWN(j) ? CH_CURS_DOWN :
             JOY_LEFT(j) ? CH_CURS_LEFT : CH_CURS_RIGHT;
}

void waitKey(void)
{
	revers(1);
	textcolor(BACKC);
	cputs("PRESS A KEY");
	revers(0);

    while (1)
    {
        if (keyHit())
        {
            cgetc();
            break;
        }

        if (getJoy())
        {
            break;
        }
    }
}

void waitRelease(void)
{
    while (1)
    {
        if (keyHit())
        {
            cgetc();
        }
        else if (!getJoy())
        {
            break;
        }
    }
}
