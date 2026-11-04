#!/usr/bin/env python3

# Build a bank test CRT for the 8K bank cartridge types that can use the
# PSRAM (images larger than crt_buf): Ocean (5), C64 Game System (15), Magic
# Desk (19), GMod2 (60).
#
# Bank 0 holds the test program (started by the KERNAL through the CBM80
# signature) and a table with the XOR of every 256 byte page of the other
# banks, which hold pseudo random data. The program runs from RAM: it
# selects banks in pseudo random order ($DE00 write: Ocean, Magic Desk, GMod2; any
# access to $DE00+bank: C64 Game System), XORs the 32 pages at $8000-$9FFF
# and compares them with the table. Shown on the screen: banks checked,
# errors, last bank with an error (border red after an error).
#
# usage: mkcart_banktest.py ocean|c64gs|magicdesk|gmod2 [file.crt]
#   ocean: 64 banks (512K), c64gs: 64 banks (512K), magicdesk: 128 banks (1 MB),
#   gmod2: 64 banks (512K)
import random, struct, sys

from mkcart_buscal import Assembler, image

TYPES = {'ocean': (5, 64, 'OCEAN'), 'c64gs': (15, 64, 'C64GS'), 'magicdesk': (19, 128, 'MAGIC DESK'),
         'gmod2': (60, 64, 'GMOD2')}

SRC = r'''
TABLE   = $1000         ; XOR table copy (banks * 32 bytes)
CODE    = $C000
SCREEN  = $0400
lfsr    = $02
bank    = $03
pg      = $04
tptr    = $FB           ; table pointer
cnt     = $0340         ; banks checked (16 bit)
errs    = $0342         ; errors (16 bit)
lastb   = $0344

        .org $8000
        .word cold, cold
        .byte $C3, $C2, $CD, $38, $30   ; CBM80
cold:   sei
        ldx #$FF
        txs
        cld
        jsr $FDA3       ; IOINIT
        jsr $FD50       ; RAMTAS
        jsr $FD15       ; RESTOR
        jsr $FF5B       ; CINT
        sei
        ldx #0
cp:     lda progimg,x
        sta CODE,x
        lda progimg+$100,x
        sta CODE+$100,x
        lda progimg+$200,x
        sta CODE+$200,x
        lda progimg+$300,x
        sta CODE+$300,x
        inx
        bne cp
        ; table: TBL_PAGES pages
        lda #<tblimg
        sta tptr
        lda #>tblimg
        sta tptr+1
        lda #<TABLE
        sta $FD
        lda #>TABLE
        sta $FE
        ldx #TBL_PAGES
        ldy #0
ct:     lda (tptr),y
        sta ($FD),y
        iny
        bne ct
        inc tptr+1
        inc $FE
        dex
        bne ct
        jmp CODE
'''

PROG = r'''
        .org $C000
start:  ldx #0
tx:     lda text,x
        sta SCREEN,x
        lda #1
        sta $D800,x
        inx
        cpx #160
        bne tx
        lda #0
        sta cnt
        sta cnt+1
        sta errs
        sta errs+1
        sta lastb
        lda #1
        sta lfsr

next:   ; next pseudo random bank, 1..NBANKS-1
        lda lfsr
        asl a
        bcc nx1
        eor #$1D
nx1:    sta lfsr
        and #NBANKS-1
        beq next
        sta bank
        jsr select
        ; table pointer = TABLE + bank * 32
        lda bank
        sta tptr
        lda #0
        sta tptr+1
        ldx #5
sh:     asl tptr
        rol tptr+1
        dex
        bne sh
        clc
        lda tptr+1
        adc #>TABLE
        sta tptr+1
        lda #$80
        sta xp+2
        ldy #0
        sty pg
pgl:    lda #0
        tax
        ldy #0
xp:     eor $8000,y
        iny
        bne xp
        ldy pg
        cmp (tptr),y
        beq pgok
        inc errs
        bne e1
        inc errs+1
e1:     lda bank
        sta lastb
        lda #2
        sta $D020
pgok:   inc xp+2
        inc pg
        lda pg
        cmp #32
        bne pgl
        inc cnt
        bne c1
        inc cnt+1
c1:     ; counters on the screen
        lda cnt+1
        ldx #52
        jsr hex
        lda cnt
        ldx #54
        jsr hex
        lda errs+1
        ldx #92
        jsr hex
        lda errs
        ldx #94
        jsr hex
        lda lastb
        ldx #137
        jsr hex
        jmp next

; A as two hex digits at SCREEN+X
hex:    pha
        lsr a
        lsr a
        lsr a
        lsr a
        jsr dig
        sta SCREEN,x
        pla
        and #$0F
        jsr dig
        sta SCREEN+1,x
        rts
dig:    cmp #10
        bcc d1
        sbc #9
        rts
d1:     ora #$30
        rts
'''

SELECT = {
    'ocean': '''
select: lda bank
        sta $DE00
        rts
''',
    'magicdesk': '''
select: lda bank
        sta $DE00
        rts
''',
    'gmod2': '''
select: lda bank        ; bits 7-6 = 00: 8K mode
        sta $DE00
        rts
''',
    'c64gs': '''
select: ldx bank
        sta $DE00,x
        rts
''',
}


def screen(text):
    out = bytearray()
    for ch in text.ljust(40):
        c = ord(ch)
        out.append(c - 64 if 'A' <= ch <= 'Z' else c)
    return out


def build(kind):
    hwtype, nbanks, label = TYPES[kind]
    rnd = random.Random(nbanks * 31 + hwtype)
    banks = [None] + [bytearray(rnd.getrandbits(8) for _ in range(8192)) for _ in range(nbanks - 1)]
    table = bytearray(32)                 # bank 0 not checked
    for b in range(1, nbanks):
        for p in range(32):
            x = 0
            for v in banks[b][p * 256:(p + 1) * 256]:
                x ^= v
            table.append(x)
    tbl_pages = (len(table) + 255) // 256
    text = screen('%s %dK BANK TEST' % (label, nbanks * 8)) + screen('BANKS:      0000') + \
           screen('ERRORS:     0000') + screen('LAST ERROR: BANK 00')
    syms = {'NBANKS': nbanks, 'TBL_PAGES': tbl_pages}
    prog_src = SRC.split('.org $8000')[0] + PROG + SELECT[kind] + '\ntext:\n'
    prog = Assembler(prog_src + '\n'.join('.byte %d' % v for v in text[:160]), syms).assemble()
    prog_img = image(prog, 0xC000, 0x400)
    assert max(prog) < 0xC400, 'program too large'
    # bank 0: boot, program image, table
    boot_src = SRC + 'progimg:\n' + '\n'.join('.byte %d' % v for v in prog_img) + '\ntblimg:\n'
    boot = Assembler(boot_src, syms).assemble()
    b0 = image(boot, 0x8000, 0x2000, 0xFF)
    tbl_at = max(boot) + 1 - 0x8000
    assert tbl_at + len(table) <= 0x2000, 'bank 0 full'
    b0[tbl_at:tbl_at + len(table)] = table
    banks[0] = b0
    return hwtype, label, banks


def write_crt(path, hwtype, label, banks):
    exrom, game = 0, 1      # 8K mode (Ocean > 128K: 16K, set by GOCart.64)
    hdr = b'C64 CARTRIDGE   ' + struct.pack('>IHHBB', 0x40, 0x0100, hwtype, exrom, game) + bytes(6)
    hdr += ('GOCART %s BANK TEST' % label).encode()[:32].ljust(32, b'\0')
    out = bytearray(hdr)
    for b, data in enumerate(banks):
        out += b'CHIP' + struct.pack('>IHHHH', 0x10 + 8192, 0, b, 0x8000, 8192) + data
    open(path, 'wb').write(out)


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in TYPES:
        raise SystemExit('usage: mkcart_banktest.py ocean|c64gs|magicdesk|gmod2 [file.crt]')
    kind = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else '%s_bank_test.crt' % kind
    write_crt(path, *build(kind))
    print(path)
