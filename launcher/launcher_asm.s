;
; Copyright (c) 2019-2024 Kim Jørgensen
;
; This software is provided 'as-is', without any express or implied
; warranty.  In no event will the authors be held liable for any damages
; arising from the use of this software.
;
; Permission is granted to anyone to use this software for any purpose,
; including commercial applications, and to alter it and redistribute it
; freely, subject to the following restrictions:
;
; 1. The origin of this software must not be misrepresented; you must not
;    claim that you wrote the original software. If you use this software
;    in a product, an acknowledgment in the product documentation would be
;    appreciated but is not required.
; 2. Altered source versions must be plainly marked as such, and must not be
;    misrepresented as being the original software.
; 3. This notice may not be removed or altered from any source distribution.
;

.importzp   ptr1
.importzp   tmp1, tmp2
.import     popax

.import     kff_send_reply

KFF_DATA    = $de00
KFF_COMMAND = $de01
KFF_RAM_TST = $de03

; Align with commands.h
CMD_SYNC        = $55
REPLY_OK        = $80

; =============================================================================
;
; void __fastcall__ kff_send_size_data(void *data, uint8_t size);
;
; =============================================================================
.proc   _kff_send_size_data
.export _kff_send_size_data
_kff_send_size_data:
        sta tmp1                ; save size

        jsr popax               ; save buffer address
        sta ptr1
        stx ptr1 + 1

        ldy #0
        ldx tmp1
        stx KFF_DATA            ; send the size
        beq @end

@loop:  lda (ptr1),y            ; send the bytes
        sta KFF_DATA
        iny
        dex
        bne @loop

@end:   rts
.endproc

; =============================================================================
;
; void __fastcall__ kff_receive_data(void *data, uint16_t size);
;
; =============================================================================
.proc   _kff_receive_data
.export _kff_receive_data
_kff_receive_data:
        sta tmp1
        stx tmp2                ; save size

        jsr popax               ; save buffer address
        sta ptr1
        stx ptr1 + 1

        ldy #0
        ldx tmp2
        beq @last

@loop1: lda KFF_DATA            ; receive a page at a time
        sta (ptr1),y
        iny
        bne @loop1
        inc ptr1 + 1
        dex
        bne @loop1

@last:  ldx tmp1
        beq @end

@loop2: lda KFF_DATA            ; receive the remaining bytes
        sta (ptr1),y
        iny
        dex
        bne @loop2

@end:   rts
.endproc

; =============================================================================
;
; uint8_t __fastcall__ kff_send_reply(uint8_t reply);
;
; =============================================================================
.proc   _kff_send_reply
.export _kff_send_reply
_kff_send_reply:
        ldx #$00                ; return value required to be 16-bit
        jmp kff_send_reply
.endproc

; =============================================================================
;
; void kff_wait_for_sync(void);
;
; This method runs from RAM to allow the cartridge to be temporary disabled
;
; =============================================================================
.data
.proc   _kff_wait_for_sync
.export _kff_wait_for_sync
_kff_wait_for_sync:
        lda #REPLY_OK                   ; Send reply
        sta KFF_COMMAND

@sync:  lda #CMD_SYNC                   ; Wait for sync command
@wait:  cmp KFF_COMMAND
        bne @wait

@next:  sta KFF_RAM_TST                 ; Check that KFF RAM is available
        cmp KFF_RAM_TST
        bne @sync
        asl
        bne @next
        rts
.endproc

; =============================================================================
;
; void wait_for_reset(void);
;
; Disable VIC-II output and wait for reset
;
; =============================================================================
.data
.proc   _wait_for_reset
.export _wait_for_reset
        sei
        lda #0
        sta $b2                         ; clear pointer to datasette buffer
        sta $b3                         ; needed for Jumpman Junior
        sta $d011                       ; disable output (needed for FC-III)
        sta $d016                       ; 38 columns

        lda #REPLY_OK
        sta KFF_COMMAND                 ; send reply

        jmp *
.endproc

; =============================================================================
;
; uint8_t is_c128(void);
;
; Return 0 if we're on a C64, other values for a C128.
;
; =============================================================================
.code
.export _is_c128
_is_c128:
        ldx $d030
        inx
        txa
        rts

; =============================================================================
;
; uint8_t read_joys(void);
;
; Joysticks in port 1 and port 2 (JOY_*_MASK bits, both ports ORed). Port 1
; shares the keyboard lines: it is read with no keyboard column selected.
; The port 1 bits are also ORed into joy1_bits (cleared by the caller).
; Fast: called many times in a row by the joystick debounce (base.c).
;
; =============================================================================
.bss
.export _joy1_bits
_joy1_bits:
        .res 1
joy_tmp:
        .res 1

.code
.export _read_joys
_read_joys:
        sei
        ldx $dc00                       ; keyboard column latch
        lda #$ff
        sta $dc00                       ; no keyboard column
        lda $dc01                       ; port 1, active low
        eor #$ff
        and #$1f
        sta joy_tmp
        ora _joy1_bits
        sta _joy1_bits
        lda $dc00                       ; port 2, active low
        eor #$ff
        and #$1f
        ora joy_tmp
        stx $dc00
        cli
        ldx #0
        rts

; =============================================================================
;
; Selection bar: the background color ($D021) of one screen row is changed
; by a raster interrupt, so the selected entry is shown with normal (not
; reversed) characters in any color on a bar of any color.
;
; void selbar_init(void);
;   selbar_color: bar color, selbar_bg: screen background color (set them
;   first). The raster interrupt replaces the CIA1 timer interrupt: once per
;   frame it draws the bar (busy waiting for the 8 raster lines of the row)
;   and then runs the KERNAL interrupt code (jiffy clock, keyboard scan).
;   Starting a program or a cartridge resets the CIAs and the VIC-II.
;
; selbar_row: screen row of the bar (1-24), SELBAR_OFF ($ff): no bar
;
; =============================================================================
SELBAR_OFF      = $ff
SELBAR_IDLE     = 250                   ; raster line of the IRQ without bar
ROW0_LINE       = 51                    ; first raster line of screen row 0

.data
.export _selbar_row
_selbar_row:
        .byte SELBAR_OFF

.bss
.export _selbar_color
_selbar_color:
        .res 1
.export _selbar_bg
_selbar_bg:
        .res 1
selbar_line:
        .res 1

.code
.export _selbar_init
_selbar_init:
        sei
        lda #$7f
        sta $dc0d                       ; no CIA1 interrupts
        lda $dc0d                       ; acknowledge a pending one
        lda #<selbar_irq
        sta $0314
        lda #>selbar_irq
        sta $0315
        jsr selbar_next
        lda #$01
        sta $d019                       ; acknowledge, enable raster IRQ
        sta $d01a
        cli
        rts

; raster compare: 3 lines before the bar (the bar starts after a busy wait,
; also if the interrupt is a bit late), or SELBAR_IDLE
selbar_next:
        lda $d011
        and #$7f                        ; raster line bit 8 = 0
        sta $d011
        lda _selbar_row
        cmp #SELBAR_OFF
        beq @idle
        asl a
        asl a
        asl a
        clc
        adc #ROW0_LINE - 3
        sta $d012
        rts
@idle:
        lda #SELBAR_IDLE
        sta $d012
        rts

; $D021 = X at the end of the raster line A (in the right border, after the
; display window): the next line is a bad line (first line of a text row),
; the 6510 is stopped from its cycle 12 on, so the color is changed before.
; The line is detected with a jitter of up to ~11 cycles: the store lands in
; cycles 56-66 of the line (display window: cycles 16-55)
selbar_at_end:
        sta selbar_line
@wait:  lda $d012
        cmp selbar_line
        bcc @wait
        ldy #9                          ; 46 cycles
@delay: dey
        bne @delay
        stx $d021
        rts

selbar_irq:
        lda $d019
        sta $d019                       ; acknowledge
        lda _selbar_row
        cmp #SELBAR_OFF
        beq @kernal
        asl a
        asl a
        asl a
        clc
        adc #ROW0_LINE - 1              ; last line of the row above
        ldx _selbar_color
        jsr selbar_at_end
        lda selbar_line
        clc
        adc #8                          ; last line of the bar row
        ldx _selbar_bg
        jsr selbar_at_end
@kernal:
        jsr selbar_next
        jmp $ea31                       ; KERNAL interrupt code
