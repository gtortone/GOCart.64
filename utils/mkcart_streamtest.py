#!/usr/bin/env python3

# Build "EF3 STREAM TEST" EasyFlash CRT: checks the reads of the EF3 USB data
# register ($DE0A) while the VIC-II steals the bus (badlines + 8 sprites).
#
# The C64 sends "KFF:" + $10 (CMD_STREAM_TEST): GOCart.64 answers with an
# endless stream 0, 1, 2... 255, 0... through $DE09/$DE0A, read with the same
# loop as the PRG loader (BIT $DE09 / BPL / LDA $DE0A). Every byte is compared
# with the expected one; after an error the test resyncs on the byte received.
#
# A read stopped by the VIC-II (BA low -> RDY low) is repeated by the 6510:
# if the cartridge applies the "byte consumed" side effect at every repeated
# read, bytes are lost (see C64_BUS_BA_READ_FILTER in include/c64_bus.h).
#
# usage: mkcart_streamtest.py [file.crt] [nosprites]
#   nosprites: sprites off (only badlines), for comparison
import struct, sys

CODE = 0x0800        # test program in RAM
CMD_STREAM_TEST = 0x10

class Asm:
    def __init__(self, org):
        self.org = org; self.b = bytearray(); self.labels = {}; self.fix = []
    def pc(self): return self.org + len(self.b)
    def L(self, name): self.labels[name] = self.pc()
    def op(self, code, arg=None, size=0, rel=False):
        self.b.append(code)
        if size == 1 and not rel:
            self.b.append(arg & 0xFF)
        elif rel:
            self.fix.append(('rel', len(self.b), arg)); self.b.append(0)
        elif size == 2:
            if isinstance(arg, str):
                self.fix.append(('abs', len(self.b), arg)); self.b += b'\0\0'
            else:
                self.b += struct.pack('<H', arg)
    def resolve(self):
        for kind, pos, lab in self.fix:
            name, add = (lab.split('+')[0], int(lab.split('+')[1])) if '+' in lab else (lab, 0)
            t = self.labels[name] + add
            if kind == 'abs':
                self.b[pos:pos+2] = struct.pack('<H', t)
            else:
                d = t - (self.org + pos + 1)
                assert -128 <= d <= 127, lab
                self.b[pos] = d & 0xFF
        return bytes(self.b)

def scr(s):
    out = bytearray()
    for c in s:
        out.append(ord(c) - 64 if 'A' <= c <= 'Z' else ord(c))
    return out

def build_program(sprites):
    a = Asm(CODE)
    ZP_EXP, ZP_CNT = 0xF9, 0xFA        # expected byte, bytes counter (low)
    a.op(0xA9, 0x2F, 1); a.op(0x85, 0x00, 1)          # CPU port DDR
    a.op(0xA9, 0x37, 1); a.op(0x85, 0x01, 1)          # BASIC+KERNAL+IO
    a.op(0xA9, 0x04, 1); a.op(0x8D, 0xDE02, 2)        # cartridge off: KERNAL visible
    a.op(0x20, 0xFF84, 2)                             # IOINIT
    a.op(0x20, 0xFD15, 2)                             # RESTOR
    a.op(0xA9, 0x04, 1); a.op(0x8D, 0x0288, 2)        # screen at $0400
    a.op(0x20, 0xFF5B, 2)                             # CINT (screen on)
    a.op(0x78)                                        # SEI
    # text
    a.op(0xA2, 0x00, 1)
    a.L('tl'); a.op(0xBD, 'text', 2); a.op(0x9D, 0x0400, 2)
    a.op(0xBD, 'text+256', 2); a.op(0x9D, 0x0500, 2); a.op(0xE8); a.op(0xD0, 'tl', rel=True)
    # counters
    a.op(0xA9, 0x00, 1); a.op(0xA2, 0x05, 1)
    a.L('cl'); a.op(0x9D, 'cnt', 2); a.op(0xCA); a.op(0x10, 'cl', rel=True)
    a.op(0x85, ZP_EXP, 1); a.op(0x85, ZP_CNT, 1)
    if sprites:
        # 8 sprites, expanded, spread over the whole display: sprite DMA on
        # every raster line of the screen
        a.op(0xA9, 0xFF, 1); a.op(0xA2, 0x3F, 1)
        a.L('sd'); a.op(0x9D, 0x0340, 2); a.op(0xCA); a.op(0x10, 'sd', rel=True)   # sprite data
        a.op(0xA2, 0x07, 1)
        a.L('sp')
        a.op(0xA9, 0x0D, 1); a.op(0x9D, 0x07F8, 2)                 # pointer: $0340
        a.op(0xBD, 'sprx', 2); a.op(0x48)
        a.op(0x8A); a.op(0x0A); a.op(0xA8)                         # Y = 2 * X
        a.op(0x68); a.op(0x99, 0xD000, 2)                          # X position
        a.op(0xBD, 'spry', 2); a.op(0x99, 0xD001, 2)               # Y position
        a.op(0xA9, 0x01, 1); a.op(0x9D, 0xD027, 2)                 # color
        a.op(0xCA); a.op(0x10, 'sp', rel=True)
        a.op(0xA9, 0x00, 1); a.op(0x8D, 0xD010, 2)
        a.op(0xA9, 0xFF, 1); a.op(0x8D, 0xD017, 2); a.op(0x8D, 0xD01D, 2)
        a.op(0x8D, 0xD015, 2)                                       # sprites on
    # "KFF:" + CMD_STREAM_TEST
    a.op(0xA2, 0x00, 1)
    a.L('tx'); a.op(0x2C, 0xDE09, 2); a.op(0x50, 'tx', rel=True)  # wait TX ready
    a.op(0xBD, 'cmd', 2); a.op(0x8D, 0xDE0A, 2)
    a.op(0xE8); a.op(0xE0, 5, 1); a.op(0xD0, 'tx', rel=True)
    # --- receive loop (same as the PRG loader)
    a.L('rx'); a.op(0x2C, 0xDE09, 2); a.op(0x10, 'rx', rel=True)  # wait RX ready
    a.op(0xAD, 0xDE0A, 2)
    a.op(0xC5, ZP_EXP, 1); a.op(0xD0, 'bad', rel=True)
    a.L('ok'); a.op(0xE6, ZP_EXP, 1)
    a.op(0xE6, ZP_CNT, 1); a.op(0xD0, 'rx', rel=True)
    # every 256 bytes: update the screen
    a.op(0xEE, 'cnt+0', 2); a.op(0xD0, 'u1', rel=True); a.op(0xEE, 'cnt+1', 2)
    a.L('u1')
    def hexout(lab, scrpos):
        a.op(0xAD, lab, 2); a.op(0x20, 'hex', 2); a.op(0x8E, scrpos, 2); a.op(0x8D, scrpos + 1, 2)
    hexout('cnt+1', 0x0400 + 5*40 + 15); hexout('cnt+0', 0x0400 + 5*40 + 17)
    hexout('cnt+3', 0x0400 + 6*40 + 15); hexout('cnt+2', 0x0400 + 6*40 + 17)
    hexout('cnt+4', 0x0400 + 7*40 + 15); hexout('cnt+5', 0x0400 + 7*40 + 27)
    a.op(0x4C, 'rx', 2)
    # error: count it, remember expected/received, resync
    a.L('bad')
    a.op(0x8D, 'cnt+5', 2)                                          # received
    a.op(0xA6, ZP_EXP, 1); a.op(0x8E, 'cnt+4', 2)                   # expected
    a.op(0x85, ZP_EXP, 1)                                           # resync
    a.op(0xEE, 'cnt+2', 2); a.op(0xD0, 'b1', rel=True); a.op(0xEE, 'cnt+3', 2)
    a.L('b1')
    a.op(0xA9, 0x02, 1); a.op(0x8D, 0xD020, 2)                      # red border
    a.op(0x4C, 'ok', 2)
    # hex: A -> X high digit, A low digit (screen codes)
    a.L('hex')
    a.op(0x48); a.op(0x4A); a.op(0x4A); a.op(0x4A); a.op(0x4A); a.op(0x20, 'dig', 2); a.op(0xAA)
    a.op(0x68); a.op(0x29, 0x0F, 1)
    a.L('dig'); a.op(0xC9, 0x0A, 1); a.op(0x90, 'd1', rel=True); a.op(0xE9, 0x09, 1); a.op(0x60)
    a.L('d1'); a.op(0x09, 0x30, 1); a.op(0x60)
    # data
    lines = ["GOCART.64 EF3 STREAM TEST ($DE0A READS)", "",
             "VIC-II DMA: BADLINES" + (" + 8 SPRITES" if sprites else " ONLY"), "", "",
             "BYTES/256:     0000", "ERRORS:        0000", "EXPECTED:      --     GOT: --"]
    text = bytearray()
    for l in lines:
        text += scr(l.ljust(40))
    a.L('text'); a.b += text.ljust(512, b' ')
    a.L('cmd'); a.b += b'KFF:' + bytes([CMD_STREAM_TEST])
    a.L('sprx'); a.b += bytes(24 + i * 28 for i in range(8))
    a.L('spry'); a.b += bytes(50 + i * 24 for i in range(8))
    a.L('cnt'); a.b += bytes(6)      # bytes/256 (16), errors (16), expected, received
    return a.resolve()

def boot_stub(prog_len):
    # bank 0 ROMH, Ultimax at $E000: copy the program from $E100 to $0800, run it
    a = Asm(0xE000)
    a.op(0x78); a.op(0xD8); a.op(0xA2, 0xFF, 1); a.op(0x9A)
    pages = (prog_len + 255) // 256
    a.op(0xA2, 0x00, 1)
    a.L('cp')
    for p in range(pages):
        a.op(0xBD, 0xE100 + p*256, 2); a.op(0x9D, CODE + p*256, 2)
    a.op(0xE8); a.op(0xD0, 'cp', rel=True)
    a.op(0x4C, CODE, 2)
    return a.resolve()

def write_crt(path, romh):
    hdr = b'C64 CARTRIDGE   ' + struct.pack('>IHHBB', 0x40, 0x0100, 32, 1, 0) + bytes(6)
    hdr += b'GOCART EF3 STREAM TEST'.ljust(32, b'\0')
    out = bytearray(hdr)
    out += b'CHIP' + struct.pack('>IHHHH', 0x10 + len(romh), 2, 0, 0xA000, len(romh)) + romh
    open(path, 'wb').write(out)

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != 'nosprites']
    sprites = 'nosprites' not in sys.argv[1:]
    prog = build_program(sprites)
    assert len(prog) <= 0x0800, len(prog)
    romh = bytearray(b'\xff' * 8192)
    stub = boot_stub(len(prog))
    romh[0:len(stub)] = stub
    romh[0x100:0x100 + len(prog)] = prog
    romh[0x1FFA:0x2000] = struct.pack('<HHH', 0xE000, 0xE000, 0xE000)
    out = args[0] if args else 'ef3_stream_test.crt'
    write_crt(out, romh)
    print('%s: program %d bytes, sprites %s' % (out, len(prog), 'on' if sprites else 'off'))
