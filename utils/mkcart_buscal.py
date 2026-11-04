#!/usr/bin/env python3

# Build the "GOCART BUS CALIBRATION" EasyFlash CRT (hardware type 32).
#
# When GOCart.64 starts a CRT with this name it does not just run it: it
# scans a grid of bus loop timing values (edge delay, hold time, see
# include/c64_bus.h), restarting the C64 with this program for every pair,
# then saves the best pair in the configuration file (see bus_calibrate() in
# src/cartridge.cpp).
#
# At every start the program receives from GOCart.64, through the EF3 USB
# registers ($DE09 status, $DE0A data), a header (mode, values under test,
# grid, results so far), shows the result map and then runs the tests:
#  - ROM reads: XOR of every 256 byte page of ROML and ROMH of banks 1-3, in
#    16K mode ($8000-$BFFF) and in Ultimax mode ($E000-$FFFF), compared with a
#    table
#  - IO2: RAM pattern written/read back at $DF00-$DFFF, then a routine run
#    from $DF00 that selects a bank ($DE00), switches 16K mode ($DE02), reads
#    ROML, switches back to Ultimax and returns: the opcode fetches from IO2
#    right after the mode changes are the critical cycles
#  - VIC-II in Ultimax mode: sprite 0 is fetched from ROMH ($3800 seen by
#    the VIC-II -> $F800, every bank has the pattern there), pattern
#    %10101010, overlapping sprite 2 (same pattern, RAM) and sprite 1
#    (%01010101, RAM): the sprite collision register must show only sprites
#    0 and 2
#  - the program itself runs from ROMH (Ultimax): a wrong opcode fetch crashes
#    it, GOCart.64 sees no result (timeout)
# The result (error count, flags) is sent back with the same registers.
#
# Memory in Ultimax mode: the CPU sees RAM only at $0000-$0FFF, so the code
# that changes the cartridge mode runs from $0800 (copied from ROM).
#
# usage: mkcart_buscal.py [file.crt]
import random, re, struct, sys

CRT_NAME = 'GOCART BUS CALIBRATION'      # recognized by the firmware

#
# minimal two-pass 6502 assembler
#
OPS = {}
def _grp(name, modes):
    for mode, code in modes.items():
        OPS[(name, mode)] = code
for n, base in (('ora', 0x00), ('and', 0x20), ('eor', 0x40), ('adc', 0x60), ('sta', 0x80),
                ('lda', 0xA0), ('cmp', 0xC0), ('sbc', 0xE0)):
    m = {'izx': base + 1, 'zp': base + 5, 'imm': base + 9, 'abs': base + 0x0D,
         'izy': base + 0x11, 'zpx': base + 0x15, 'aby': base + 0x19, 'abx': base + 0x1D}
    if n == 'sta':
        del m['imm']
    _grp(n, m)
for n, base in (('asl', 0x00), ('rol', 0x20), ('lsr', 0x40), ('ror', 0x60)):
    _grp(n, {'zp': base + 6, 'acc': base + 0x0A, 'abs': base + 0x0E, 'zpx': base + 0x16, 'abx': base + 0x1E})
_grp('inc', {'zp': 0xE6, 'abs': 0xEE, 'zpx': 0xF6, 'abx': 0xFE})
_grp('dec', {'zp': 0xC6, 'abs': 0xCE, 'zpx': 0xD6, 'abx': 0xDE})
_grp('ldx', {'imm': 0xA2, 'zp': 0xA6, 'abs': 0xAE, 'zpy': 0xB6, 'aby': 0xBE})
_grp('ldy', {'imm': 0xA0, 'zp': 0xA4, 'abs': 0xAC, 'zpx': 0xB4, 'abx': 0xBC})
_grp('stx', {'zp': 0x86, 'abs': 0x8E, 'zpy': 0x96})
_grp('sty', {'zp': 0x84, 'abs': 0x8C, 'zpx': 0x94})
_grp('cpx', {'imm': 0xE0, 'zp': 0xE4, 'abs': 0xEC})
_grp('cpy', {'imm': 0xC0, 'zp': 0xC4, 'abs': 0xCC})
_grp('bit', {'zp': 0x24, 'abs': 0x2C})
_grp('jmp', {'abs': 0x4C, 'ind': 0x6C})
_grp('jsr', {'abs': 0x20})
for n, c in (('bpl', 0x10), ('bmi', 0x30), ('bvc', 0x50), ('bvs', 0x70), ('bcc', 0x90),
             ('bcs', 0xB0), ('bne', 0xD0), ('beq', 0xF0)):
    OPS[(n, 'rel')] = c
for n, c in (('brk', 0x00), ('php', 0x08), ('clc', 0x18), ('plp', 0x28), ('sec', 0x38),
             ('rti', 0x40), ('pha', 0x48), ('cli', 0x58), ('rts', 0x60), ('pla', 0x68),
             ('sei', 0x78), ('dey', 0x88), ('txa', 0x8A), ('tya', 0x98), ('txs', 0x9A),
             ('tay', 0xA8), ('tax', 0xAA), ('clv', 0xB8), ('tsx', 0xBA), ('iny', 0xC8),
             ('dex', 0xCA), ('cld', 0xD8), ('inx', 0xE8), ('nop', 0xEA), ('sed', 0xF8)):
    OPS[(n, 'imp')] = c
SIZE = {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'izx': 2, 'izy': 2,
        'rel': 2, 'abs': 3, 'abx': 3, 'aby': 3, 'ind': 3}


def screen_codes(text):
    out = bytearray()
    for ch in text:
        c = ord(ch)
        if 'A' <= ch <= 'Z':
            out.append(c - 64)
        elif 'a' <= ch <= 'z':
            out.append(c - 96)
        elif ch == '@':
            out.append(0)
        else:
            out.append(c)        # space, digits, punctuation: same code
    return out


class Assembler:
    """Source: one statement per line, ';' comments.
       label:  |  name = expr  |  .org expr  |  .byte e,...  |  .word e,...
       .scr "text" (screen codes)  |  .fill count, value  |  op operand
       Expressions: decimal, $hex, %bin, labels, + - *, <expr, >expr, '*' = pc."""

    def __init__(self, src, symbols=None):
        self.src = src
        self.sym = dict(symbols or {})

    def value(self, expr, pc, final):
        expr = expr.strip()
        if expr.startswith('<'):
            return self.value(expr[1:], pc, final) & 0xFF
        if expr.startswith('>'):
            return (self.value(expr[1:], pc, final) >> 8) & 0xFF
        tokens = re.findall(r"\$[0-9a-fA-F]+|%[01]+|\d+|[A-Za-z_][A-Za-z0-9_]*|[-+*()]", expr)
        py = []
        for t in tokens:
            if t.startswith('$'):
                py.append(str(int(t[1:], 16)))
            elif t.startswith('%'):
                py.append(str(int(t[1:], 2)))
            elif t[0].isdigit():
                py.append(t)
            elif t[0].isalpha() or t[0] == '_':
                if t in self.sym:
                    py.append(str(self.sym[t]))
                elif final:
                    raise ValueError('undefined symbol ' + t)
                else:
                    py.append('0')
            elif t == '*' and (not py or py[-1] in ('+', '-', '*', '(')):
                py.append(str(pc))
            else:
                py.append(t)
        return int(eval(''.join(py)))

    def mode(self, op, arg, pc, final):
        if not arg:
            return ('acc' if (op, 'acc') in OPS else 'imp'), None
        if arg.lower() == 'a' and (op, 'acc') in OPS:
            return 'acc', None
        if (op, 'rel') in OPS:
            return 'rel', self.value(arg, pc, final)
        if arg.startswith('#'):
            return 'imm', self.value(arg[1:], pc, final)
        m = re.match(r'^\((.*),\s*[xX]\)$', arg)
        if m:
            return 'izx', self.value(m.group(1), pc, final)
        m = re.match(r'^\((.*)\),\s*[yY]$', arg)
        if m:
            return 'izy', self.value(m.group(1), pc, final)
        m = re.match(r'^\((.*)\)$', arg)
        if m:
            return 'ind', self.value(m.group(1), pc, final)
        idx = None
        m = re.match(r'^(.*),\s*([xXyY])$', arg)
        if m:
            arg, idx = m.group(1), m.group(2).lower()
        v = self.value(arg, pc, final)
        # zero page only for a numeric operand (labels may move between passes)
        zp_ok = re.fullmatch(r'\$[0-9a-fA-F]{1,2}|\d+', arg.strip()) is not None and v < 256
        if idx is None:
            return ('zp' if zp_ok and (op, 'zp') in OPS else 'abs'), v
        z, a = 'zp' + idx, 'ab' + idx
        return (z if zp_ok and (op, z) in OPS else a), v

    def assemble(self):
        for final in (False, True):
            out = {}
            pc = 0
            for lineno, line in enumerate(self.src.split('\n'), 1):
                line = re.sub(r";.*$", '', line).rstrip()
                if not line.strip():
                    continue
                try:
                    m = re.match(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$', line)
                    if m:
                        self.sym[m.group(1)] = self.value(m.group(2), pc, final)
                        continue
                    m = re.match(r'^\s*([A-Za-z_][A-Za-z0-9_]*):(.*)$', line)
                    if m:
                        self.sym[m.group(1)] = pc
                        line = m.group(2)
                        if not line.strip():
                            continue
                    parts = line.strip().split(None, 1)
                    op = parts[0].lower()
                    arg = parts[1].strip() if len(parts) > 1 else ''
                    if op == '.org':
                        pc = self.value(arg, pc, final)
                    elif op in ('.byte', '.word'):
                        for e in arg.split(','):
                            v = self.value(e, pc, final)
                            if op == '.byte':
                                out[pc] = v & 0xFF; pc += 1
                            else:
                                out[pc] = v & 0xFF; out[pc + 1] = (v >> 8) & 0xFF; pc += 2
                    elif op == '.scr':
                        for b in screen_codes(arg.strip('"')):
                            out[pc] = b; pc += 1
                    elif op == '.fill':
                        n, v = (self.value(e, pc, final) for e in arg.split(','))
                        for _ in range(n):
                            out[pc] = v & 0xFF; pc += 1
                    else:
                        md, v = self.mode(op, arg, pc, final)
                        if (op, md) not in OPS:
                            raise ValueError('bad mode %s for %s' % (md, op))
                        out[pc] = OPS[(op, md)]
                        if md == 'rel':
                            d = v - (pc + 2)
                            if final and not -128 <= d <= 127:
                                raise ValueError('branch out of range')
                            out[pc + 1] = d & 0xFF
                        elif SIZE[md] == 2:
                            out[pc + 1] = v & 0xFF
                        elif SIZE[md] == 3:
                            out[pc + 1] = v & 0xFF; out[pc + 2] = (v >> 8) & 0xFF
                        pc += SIZE[md]
                except Exception as e:
                    raise SystemExit('line %d: %s: %s' % (lineno, line.strip(), e))
        return out


def image(out, start, size, fill=0):
    data = bytearray([fill]) * size
    for a, v in out.items():
        if start <= a < start + size:
            data[a - start] = v
    return data


#
# test banks: 1..3, ROML and ROMH pseudo random, ROML first byte = bank number
#
TEST_BANKS = (1, 2, 3)
rnd = random.Random(0x6C64)
chips = {}
for b in TEST_BANKS:
    lo = bytearray(rnd.getrandbits(8) for _ in range(8192)); lo[0] = b
    hi = bytearray(rnd.getrandbits(8) for _ in range(8192))
    # the VIC-II fetches sprite 0 from ROMH $F800 of the selected bank also
    # while a test bank is selected in Ultimax mode: same pattern as bank 0
    hi[0x1800:0x1840] = bytes([0xAA]) * 64
    chips[b] = (lo, hi)

def page_xor(data):
    out = bytearray()
    for p in range(32):
        x = 0
        for v in data[p * 256:(p + 1) * 256]:
            x ^= v
        out.append(x)
    return out

# expected XOR per page: for each test bank 32 bytes ROML, 32 bytes ROMH
xor_table = bytearray()
for b in TEST_BANKS:
    xor_table += page_xor(chips[b][0]) + page_xor(chips[b][1])

SRC = r'''
; ---------------------------------------------------------------------------
; zero page / RAM ($0000-$0FFF is the only RAM the CPU sees in Ultimax mode)
ptr     = $FB           ; text pointer
scr     = $FD           ; screen pointer
errlo   = $F7
errhi   = $F8
flags   = $F9           ; 1 ROM, 2 IO2 RAM, 4 IO2 code, 8 sprites, 16 write bursts,
                        ; 32 ROM with other VIC-II settings
rflag   = $F0           ; flag of the ROM test errors
vsi     = $EF           ; VIC-II setting under test
rtend   = $EE           ; ROM test: last bank + 1
tmp     = $FA
tmp2    = $F6
bank    = $F5
pg      = $F4           ; table index
cnt     = $F3
pdv     = $F2           ; print_dec
pdc     = $F1

HDR     = $0200         ; header from GOCart.64
H_MAGIC = HDR+0
H_MODE  = HDR+1         ; 0 test, 1 done (result in edge/hold), 2 done, no pair found
H_EDGE  = HDR+2
H_HOLD  = HDR+3
H_IDX   = HDR+4         ; index of the pair under test
H_N     = HDR+5         ; number of grid points
H_NE    = HDR+6         ; edge values (columns)
H_NH    = HDR+7         ; hold values (rows)
H_E0    = HDR+8
H_ES    = HDR+9
H_H0    = HDR+10
H_HS    = HDR+11
H_SAVED = HDR+12        ; mode 1: config saved
HDR_LEN = 13
MAP     = $0300         ; grid results: 0 not tested, 1 pass, 2 fail (<= 255)

SPR1    = $0E00         ; sprite 1 data (pointer $38)
SPR2    = $0E40         ; sprite 2 data (pointer $39)
CHARS   = $2000         ; copy of the character ROM (VIC-II sees RAM there)

EF_BANK = $DE00
EF_CTRL = $DE02
EF_ULTIMAX = 5
EF_16K  = 7
USB_ST  = $DE09         ; bit 7: byte for the C64, bit 6: C64 may send
USB_DATA = $DE0A

; ---------------------------------------------------------------------------
        .org $E000      ; bank 0 ROMH, Ultimax mode
reset:
        sei
        cld
        ldx #$FF
        txs
        lda #$2F
        sta $00
        lda #$37
        sta $01
        lda #$7F
        sta $DC0D
        sta $DD0D
        lda $DC0D
        lda $DD0D
        lda #$03
        sta $DD02
        sta $DD00       ; VIC-II bank 0
        lda #0
        sta $D015
        sta $D01A
        lda #$1B
        sta $D011
        lda #$08
        sta $D016
        lda #$18        ; screen $0400, characters $2000
        sta $D018
        lda #14
        sta $D020
        lda #6
        sta $D021

        ; RAM code to $0800
        ldx #0
rcopy:  lda ramimg,x
        sta $0800,x
        lda ramimg+$100,x
        sta $0900,x
        lda ramimg+$200,x
        sta $0A00,x
        lda ramimg+$300,x
        sta $0B00,x
        inx
        bne rcopy
        jsr r_chars     ; character ROM -> $2000, sprite shadow $3800

        ; clear screen, color RAM white
        ldx #0
cls:    lda #32
        sta $0400,x
        sta $0500,x
        sta $0600,x
        sta $06E8,x
        lda #1
        sta $D800,x
        sta $D900,x
        sta $DA00,x
        sta $DAE8,x
        inx
        bne cls

        ; sprites: 0 from ROMH ($3800 -> $F800), 1 and 2 from RAM, overlapping
        ldx #62
sprd:   lda #$55
        sta SPR1,x
        lda #$AA
        sta SPR2,x
        dex
        bpl sprd
        lda #$E0
        sta $07F8
        lda #$38
        sta $07F9
        lda #$39
        sta $07FA
        ldx #0
spos:   lda #$E8
        sta $D000,x
        lda #$E5
        sta $D001,x
        inx
        inx
        cpx #6
        bne spos
        lda #0
        sta $D010
        sta $D017
        sta $D01D
        sta $D01B
        sta $D01C
        lda #1
        sta $D027
        sta $D028
        sta $D029
        lda #$07
        sta $D015

        ; title
        lda #<t_title
        ldx #>t_title
        ldy #0
        jsr print_row

        ; header from GOCart.64
        ldx #0
rhdr:   jsr getc
        sta HDR,x
        inx
        cpx #HDR_LEN
        bne rhdr
        lda H_MAGIC
        cmp #$C5
        beq hdr_ok
        lda #<t_proto
        ldx #>t_proto
        ldy #22
        jsr print_row
hang:   jmp hang
hdr_ok: ldx #0
rmap:   cpx H_N
        beq map_done
        jsr getc
        sta MAP,x
        inx
        bne rmap
map_done:
        jsr draw_map

        lda H_MODE
        beq testing
        cmp #1
        bne nopair
        lda #<t_result
        ldx #>t_result
        ldy #20
        jsr print_row
        lda H_EDGE
        ldy #20
        ldx #11
        jsr print_dec
        lda H_HOLD
        ldy #20
        ldx #19
        jsr print_dec
        lda H_SAVED
        beq notsaved
        lda #<t_saved
        ldx #>t_saved
        jmp lastline
notsaved:
        lda #<t_notsaved
        ldx #>t_notsaved
lastline:
        ldy #22
        jsr print_row
        jmp hang
nopair: lda #<t_nopair
        ldx #>t_nopair
        ldy #20
        jsr print_row
        jmp hang

testing:
        lda #<t_testing
        ldx #>t_testing
        ldy #20
        jsr print_row
        lda H_EDGE
        ldy #20
        ldx #13
        jsr print_dec
        lda H_HOLD
        ldy #20
        ldx #21
        jsr print_dec

        jsr r_tests     ; in RAM: changes the cartridge mode

        ; result to GOCart.64: 'R', errors (16 bit), flags
        lda #$52
        jsr putc
        lda errlo
        jsr putc
        lda errhi
        jsr putc
        lda flags
        jsr putc

        lda errlo
        ora errhi
        bne tfail
        lda #<t_pass
        ldx #>t_pass
        jmp tres
tfail:  lda #<t_fail
        ldx #>t_fail
tres:   ldy #22
        jsr print_row
        jmp hang

; byte from GOCart.64
getc:   bit USB_ST
        bpl getc
        lda USB_DATA
        rts

; byte to GOCart.64
putc:   bit USB_ST
        bvc putc
        sta USB_DATA
        rts

; screen row address of row y -> scr
rowaddr:
        lda #0
        sta scr
        lda #$04
        sta scr+1
ra1:    cpy #0
        beq ra2
        clc
        lda scr
        adc #40
        sta scr
        bcc ra3
        inc scr+1
ra3:    dey
        jmp ra1
ra2:    rts

; text at A/X (screen codes, 0 terminated) to row Y, column 0
print_row:
        sta ptr
        stx ptr+1
        jsr rowaddr
        ldy #0
pr1:    lda (ptr),y
        beq pr2
        sta (scr),y
        iny
        bne pr1
pr2:    rts

; A (0-99) as two decimal digits at row Y, column X
print_dec:
        sta pdv
        stx pdc
        jsr rowaddr
        lda pdv
        ldx #$30
pd1:    cmp #10
        bcc pd2
        sbc #10
        inx
        jmp pd1
pd2:    ora #$30
        ldy pdc
        iny
        sta (scr),y
        txa
        dey
        sta (scr),y
        rts

; result map: rows = hold values (screen rows 5..), columns = edge values
draw_map:
        lda #<t_axis
        ldx #>t_axis
        ldy #3
        jsr print_row
        lda H_E0
        ldy #3
        ldx #10
        jsr print_dec
        lda H_NE        ; last edge = e0 + (ne-1)*es
        sec
        sbc #1
        tax
        lda H_E0
dm0:    cpx #0
        beq dm0e
        clc
        adc H_ES
        dex
        jmp dm0
dm0e:   ldy #3
        ldx #15
        jsr print_dec
        lda H_ES
        ldy #3
        ldx #24
        jsr print_dec

        lda #0
        sta pg          ; row
        sta cnt         ; grid index
        lda H_H0
        sta bank        ; hold value of the row
dm1:    lda pg
        cmp H_NH
        beq dmx
        clc
        adc #5
        sta tmp         ; screen row
        tay
        lda bank
        ldx #0
        jsr print_dec
        ldy tmp
        jsr rowaddr
        ldy #4
        ldx #0
dm2:    stx tmp2
        ldx cnt
        lda MAP,x
        cpx H_IDX
        bne dm3
        lda H_MODE
        bne dm3a
        lda #42         ; '*': under test
        jmp dm4
dm3a:   lda MAP,x
dm3:    tax
        lda cells,x
dm4:    sta (scr),y
        iny
        iny
        inc cnt
        ldx tmp2
        inx
        cpx H_NE
        bne dm2
        inc pg
        clc
        lda bank
        adc H_HS
        sta bank
        jmp dm1
dmx:    rts

cells:  .byte 45, 15, 24       ; '-' not tested, 'O' pass, 'X' fail

t_title:    .scr "GOCART.64 BUS CALIBRATION"
            .byte 0
t_axis:     .scr "HOLD EDGE    -     STEP"
            .byte 0
t_testing:  .scr "TESTING EDGE    HOLD"
            .byte 0
t_result:   .scr "BEST: EDGE    HOLD"
            .byte 0
t_saved:    .scr "SAVED IN THE CONFIGURATION"
            .byte 0
t_notsaved: .scr "NOT SAVED (SD CARD?)"
            .byte 0
t_nopair:   .scr "NO WORKING PAIR FOUND"
            .byte 0
t_pass:     .scr "PASS"
            .byte 0
t_fail:     .scr "FAIL"
            .byte 0
t_proto:    .scr "PROTOCOL ERROR"
            .byte 0

nmi:    rti

ramimg:                 ; assembled for $0800, copied there at reset
'''

RAM_SRC = r'''
        .org $0800
; character ROM -> $2000 (16K mode, $01 = $33), sprite 0 shadow at $3800
; (in 16K mode the VIC-II sees RAM at $3800: same pattern as in ROMH)
r_chars:
        lda #EF_16K
        sta EF_CTRL
        lda #$33
        sta $01
        ldx #0
rc1:    lda $D000,x
        sta CHARS,x
        lda $D100,x
        sta CHARS+$100,x
        lda $D200,x
        sta CHARS+$200,x
        lda $D300,x
        sta CHARS+$300,x
        lda $D400,x
        sta CHARS+$400,x
        lda $D500,x
        sta CHARS+$500,x
        lda $D600,x
        sta CHARS+$600,x
        lda $D700,x
        sta CHARS+$700,x
        inx
        bne rc1
        lda #$37
        sta $01
        ldx #63
        lda #$AA
rc2:    sta $3800,x
        dex
        bpl rc2
        lda #EF_ULTIMAX
        sta EF_CTRL
        rts

; all the tests, errors in errlo/errhi, flags
r_tests:
        lda #0
        sta errlo
        sta errhi
        sta flags

        ; ROM with the usual VIC-II setting (bank 0, display on)
        lda #1
        sta rflag
        lda #4          ; banks 1-3
        sta rtend
        jsr r_romtest

        ; ROM again with other VIC-II settings: the address the VIC-II puts
        ; on the bus in its phase (display off: idle fetches at $3FFF of its
        ; bank, $FFFF in bank 3) is the one the bus changes from when the
        ; CPU phase starts. A ROM read served too early (short edge delay)
        ; may depend on it: seen with an EasyFlash game (VIC-II bank 3)
        lda #32
        sta rflag
        lda #2          ; bank 1 only (time)
        sta rtend
        ldx #0
vs1:    stx vsi
        lda vs_dd00,x
        sta $DD00
        lda vs_d011,x
        sta $D011
        jsr r_romtest
        ldx vsi
        inx
        cpx #vs_d011-vs_dd00
        bne vs1
        lda #$03        ; back to bank 0, display on
        sta $DD00
        lda #$1B
        sta $D011
        lda #1
        sta rflag
        lda $D01E       ; clear the sprite collisions

        ; IO2 RAM pattern, 4 passes
        lda #4
        sta cnt
io1:    ldx #0
io2:    txa
        eor cnt
        sta $DF00,x
        inx
        bne io2
io3:    txa
        eor cnt
        cmp $DF00,x
        beq io4
        lda #2
        jsr r_err
io4:    inx
        bne io3
        dec cnt
        bne io1

        ; routine run from IO2: bank, 16K mode, ROML read, Ultimax
        ldx #0
io5:    lda io2code,x
        sta $DF00,x
        inx
        cpx #io2end-io2code
        bne io5
        lda #0
        sta cnt         ; 256 calls, banks 1, 2, 3...
        lda #1
        sta bank
io6:    lda bank
        jsr $DF00       ; returns the first ROML byte of the bank
        cmp bank
        beq io7
        lda #4
        jsr r_err
io7:    inc bank
        lda bank
        cmp #4
        bne io8
        lda #1
        sta bank
io8:    dec cnt
        bne io6

        ; write bursts to I/O registers: no spurious cartridge register write
        jsr r_wburst

        ; sprite collisions: 0 and 2 only
        lda $D01E
        and #$07
        cmp #$05
        beq rtx
        lda #8
        jsr r_err
rtx:    rts

; ROM: banks 1 to rtend - 1, 16K mode ($8000-$BFFF) then Ultimax
; ($E000-$FFFF), errors with flag rflag; ends in Ultimax mode, bank 0
r_romtest:
        lda #0
        sta pg
        lda #1
        sta bank
rt1:    lda bank
        sta EF_BANK
        lda #EF_16K
        sta EF_CTRL
        lda #$80
        ldx pg
        jsr r_xor32     ; ROML
        lda #$A0
        jsr r_xor32     ; ROMH at $A000
        txa
        sec
        sbc #32
        tax             ; same ROMH table
        lda #EF_ULTIMAX
        sta EF_CTRL
        lda #$E0
        jsr r_xor32     ; ROMH at $E000
        stx pg
        inc bank
        lda bank
        cmp rtend
        bne rt1
        lda #0
        sta EF_BANK     ; back to bank 0 (this code's ROMH)
        rts

; VIC-II settings for the ROM test: $DD00 (VIC-II bank = 3 - value), $D011
vs_dd00:
        .byte $03, $00, $00, $02, $01
vs_d011:
        .byte $0B, $1B, $0B, $0B, $0B

; Write bursts to harmless I/O registers in $D000-$DDFF (SID and mirrors,
; border color, CIA timer latches with the timers stopped, unused color RAM)
; in 16K mode, banks 1-3, with the VIC-II showing the screen and 3 sprites.
; A select line glitch while the address changes at the start of the CPU
; phase (IO1/IO2 seen for a moment, on a breadbin with a GAL PLA) must not
; become a register write: a short edge delay does not mask it. Checked
; after each burst: same bank in 16K mode ($8000 = bank number, XOR of the
; first ROMH page), then IO2 RAM unchanged. Ends in Ultimax mode, bank 0.
WB_LOOPS = 128
r_wburst:
        ldx #0
wb0:    txa
        eor #$A5
        sta $DF00,x     ; IO2 RAM pattern
        inx
        bne wb0
        lda #1
        sta bank
wb1:    lda bank
        sta EF_BANK
        lda #EF_16K
        sta EF_CTRL
        lda #WB_LOOPS
        sta cnt
wb2:    ldx #24
wb3:    lda #0
        sta $D400,x     ; SID and its mirrors (volume 0)
        sta $D500,x
        sta $D600,x
        sta $D700,x
        lda #14
        sta $D020
        txa
        sta $DC04       ; CIA timer latches
        sta $DD04
        sta $DD06
        sta $DBE7,x     ; color RAM after the screen ($DBE8-$DBFF)
        dex
        bpl wb3
        dec cnt
        bne wb2
        lda $8000       ; ROML: bank number
        cmp bank
        bne wbe
        lda #0          ; ROMH: XOR of the first page
        tay
wb4:    eor $A000,y
        iny
        bne wb4
        sta tmp
        lda bank        ; ROMH table entry: bank * 64 - 32
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        sec
        sbc #32
        tax
        lda tmp
        cmp xortab,x
        beq wb5
wbe:    lda #16
        jsr r_err
wb5:    inc bank
        lda bank
        cmp #4
        bne wb1
        ldx #0          ; IO2 RAM unchanged
wb6:    txa
        eor #$A5
        cmp $DF00,x
        beq wb7
        lda #16
        jsr r_err
wb7:    inx
        bne wb6
        lda #0
        sta EF_BANK
        lda #EF_ULTIMAX
        sta EF_CTRL
        rts

; XOR of 32 pages from page A, each compared with xortab,x (x advanced)
r_xor32:
        sta xpg+2
        lda #32
        sta tmp
xr1:    lda #0
        ldy #0
xpg:    eor $8000,y     ; page patched above
        iny
        bne xpg
        cmp xortab,x
        beq xr2
        lda rflag
        jsr r_err
xr2:    inx
        inc xpg+2
        dec tmp
        bne xr1
        rts

; one more error, flag A
r_err:  ora flags
        sta flags
        inc errlo
        bne re1
        inc errhi
re1:    rts

; copied to $DF00: A = bank -> A = first ROML byte of the bank
io2code:
        sta EF_BANK
        ldx #EF_16K
        stx EF_CTRL
        lda $8000
        ldx #EF_ULTIMAX
        stx EF_CTRL
        ldx #0
        stx EF_BANK
        rts
io2end:

xortab:
'''


def build():
    # RAM image first (its labels are used by the ROM code)
    ram_syms = {}
    pre = SRC.split('.org $E000')[0]
    a = Assembler(pre + RAM_SRC + '\n' + '\n'.join('.byte %d' % v for v in xor_table))
    ram = a.assemble()
    ram_syms = {k: v for k, v in a.sym.items()}
    ram_img = image(ram, 0x0800, 0x400)
    ram_end = max(ram) + 1
    assert ram_end <= 0x0C00, 'RAM code too large: $%04X' % ram_end
    assert ram_end <= 0x0E00

    rom = Assembler(SRC + '\n'.join('.byte %d' % v for v in ram_img),
                    {k: v for k, v in ram_syms.items() if k.startswith('r_')}).assemble()
    rom_end = max(rom) + 1
    assert rom_end <= 0xF800, 'ROM code too large: $%04X' % rom_end
    romh0 = image(rom, 0xE000, 0x2000, 0xFF)
    # sprite 0 data at VIC-II $3800 = ROMH $F800 (Ultimax)
    romh0[0x1800:0x1800 + 64] = bytes([0xAA]) * 64
    labels = Assembler(SRC + '\n'.join('.byte %d' % v for v in ram_img),
                       {k: v for k, v in ram_syms.items() if k.startswith('r_')})
    labels.assemble()
    struct.pack_into('<HHH', romh0, 0x1FFA, labels.sym['nmi'], labels.sym['reset'], labels.sym['nmi'])
    roml0 = bytearray([0xFF]) * 0x2000
    roml0[0] = 0
    return roml0, romh0


def write_crt(path, roml0, romh0):
    hdr = b'C64 CARTRIDGE   ' + struct.pack('>IHHBB', 0x40, 0x0100, 32, 1, 0) + bytes(6)
    hdr += CRT_NAME.encode().ljust(32, b'\0')
    out = bytearray(hdr)
    def chip(bank, addr, data):
        return b'CHIP' + struct.pack('>IHHHH', 0x10 + 8192, 2, bank, addr, 8192) + data
    out += chip(0, 0x8000, roml0) + chip(0, 0xA000, romh0)
    for b in TEST_BANKS:
        out += chip(b, 0x8000, chips[b][0]) + chip(b, 0xA000, chips[b][1])
    open(path, 'wb').write(out)


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'bus_calibration.crt'
    write_crt(path, *build())
    print(path)
