#!/usr/bin/env python3

# Build "EF READ TEST" EasyFlash CRT: checks 16K-mode ROM reads on every bank
import random, struct, sys

NBANKS = 16          # banks tested (ROML+ROMH each)
CODE = 0x0800        # test program in RAM

class Asm:
    def __init__(self, org):
        self.org = org; self.b = bytearray(); self.labels = {}; self.fix = []
    def pc(self): return self.org + len(self.b)
    def L(self, name): self.labels[name] = self.pc()
    def raw(self, *bs): self.b += bytes(bs)
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

def build_program(exp):
    a = Asm(CODE)
    ZP_PTR, ZP_SUM, ZP_BANK, ZP_SCR = 0xFB, 0xF7, 0xF9, 0xFD
    a.op(0xA9, 0x2F, 1); a.op(0x85, 0x00, 1)          # CPU port DDR
    a.op(0xA9, 0x37, 1); a.op(0x85, 0x01, 1)          # BASIC+KERNAL+IO
    a.op(0xA9, 0x87, 1); a.op(0x8D, 0xDE02, 2)        # 16K mode: KERNAL visible
    a.op(0x20, 0xFF84, 2)                             # IOINIT
    a.op(0x20, 0xFD15, 2)                             # RESTOR
    a.op(0xA9, 0x04, 1); a.op(0x8D, 0x0288, 2)        # screen at $0400
    a.op(0x20, 0xFF5B, 2)                             # CINT (screen on)
    a.op(0x78)                                        # SEI
    a.op(0xA9, 0x04, 1); a.op(0x8D, 0xDE02, 2)        # cartridge off
    # clear counters
    a.op(0xA2, 0x00, 1); a.op(0xA9, 0x00, 1)
    a.L('clr'); a.op(0x9D, 'cnt', 2); a.op(0xE8); a.op(0xE0, 6 + NBANKS, 1); a.op(0xD0, 'clr', rel=True)
    # title text
    a.op(0xA2, 0x00, 1)
    a.L('tl'); a.op(0xBD, 'text', 2); a.op(0x9D, 0x0400, 2)
    a.op(0xBD, 'text+256', 2); a.op(0x9D, 0x0500, 2); a.op(0xE8); a.op(0xD0, 'tl', rel=True)
    # --- main loop
    a.L('pass')
    a.op(0xA9, 0x00, 1); a.op(0x85, ZP_BANK, 1)
    a.L('bank')
    a.op(0xA9, 0x87, 1); a.op(0x8D, 0xDE02, 2)        # 16K on (like Snow Strike loader)
    a.op(0xA5, ZP_BANK, 1); a.op(0x8D, 0xDE00, 2)
    a.op(0xA9, 0x00, 1); a.op(0x85, ZP_SUM, 1); a.op(0x85, ZP_SUM+1, 1); a.op(0x85, ZP_PTR, 1)
    a.op(0xA9, 0x80, 1); a.op(0x85, ZP_PTR+1, 1)
    a.op(0xA0, 0x00, 1)
    a.L('sum'); a.op(0xB1, ZP_PTR, 1); a.op(0x18); a.op(0x65, ZP_SUM, 1); a.op(0x85, ZP_SUM, 1)
    a.op(0x90, 'nc', rel=True); a.op(0xE6, ZP_SUM+1, 1)
    a.L('nc'); a.op(0xC8); a.op(0xD0, 'sum', rel=True)
    a.op(0xE6, ZP_PTR+1, 1); a.op(0xA5, ZP_PTR+1, 1); a.op(0xC9, 0xC0, 1); a.op(0xD0, 'sum', rel=True)
    a.op(0xA9, 0x04, 1); a.op(0x8D, 0xDE02, 2)        # cartridge off
    # compare with the expected sum
    a.op(0xA6, ZP_BANK, 1)
    a.op(0xA5, ZP_SUM, 1); a.op(0xDD, 'explo', 2); a.op(0xD0, 'bad', rel=True)
    a.op(0xA5, ZP_SUM+1, 1); a.op(0xDD, 'exphi', 2); a.op(0xF0, 'next', rel=True)
    a.L('bad')
    a.op(0xEE, 'cnt+2', 2); a.op(0xD0, 'b1', rel=True); a.op(0xEE, 'cnt+3', 2)   # errors (16 bit)
    a.L('b1')
    a.op(0xBD, 'cnt+6', 2); a.op(0xC9, 0x09, 1); a.op(0xF0, 'b2', rel=True); a.op(0xFE, 'cnt+6', 2)  # per bank, max 9
    a.L('b2')
    a.op(0x8E, 'cnt+4', 2)                             # last bad bank
    a.op(0xBD, 'cnt+6', 2); a.op(0x09, 0x30, 1); a.op(0x9D, 0x0400 + 9*40, 2)
    a.op(0xA9, 0x02, 1); a.op(0x8D, 0xD020, 2)         # red border
    a.L('next')
    a.op(0xE6, ZP_BANK, 1); a.op(0xA5, ZP_BANK, 1); a.op(0xC9, NBANKS, 1); a.op(0xF0, 'pdone', rel=True)
    a.op(0x4C, 'bank', 2)
    a.L('pdone')
    a.op(0xEE, 'cnt+0', 2); a.op(0xD0, 'p1', rel=True); a.op(0xEE, 'cnt+1', 2)
    a.L('p1')
    # print counters: pass at row 5 col 7, errors row 6 col 7, last bad bank row 7 col 12
    def hexout(lab_hi, lab_lo, scr):
        for i, lab in enumerate((lab_hi, lab_lo)):
            if lab is None: continue
            a.op(0xAD, lab, 2); a.op(0x20, 'hex', 2)
            a.op(0x8E, scr + 2*i, 2); a.op(0x8D, scr + 2*i + 1, 2)
    hexout('cnt+1', 'cnt+0', 0x0400 + 5*40 + 12)
    hexout('cnt+3', 'cnt+2', 0x0400 + 6*40 + 12)
    a.op(0xAD, 'cnt+2', 2); a.op(0x0D, 'cnt+3', 2); a.op(0xF0, 'p2', rel=True)
    hexout(None, 'cnt+4', 0x0400 + 7*40 + 10)
    a.L('p2')
    a.op(0x4C, 'pass', 2)
    # hex: A -> X high digit, A low digit (screen codes)
    a.L('hex')
    a.op(0x48); a.op(0x4A); a.op(0x4A); a.op(0x4A); a.op(0x4A); a.op(0x20, 'dig', 2); a.op(0xAA)
    a.op(0x68); a.op(0x29, 0x0F, 1)
    a.L('dig'); a.op(0xC9, 0x0A, 1); a.op(0x90, 'd1', rel=True); a.op(0xE9, 0x09, 1); a.op(0x60)
    a.L('d1'); a.op(0x09, 0x30, 1); a.op(0x60)
    # data
    def scr(s):
        out = bytearray()
        for c in s:
            if 'A' <= c <= 'Z': out.append(ord(c) - 64)
            else: out.append(ord(c))
        return out
    lines = ["GOCART.64 EASYFLASH 16K READ TEST", "",
             "%d BANKS X 16K, CHECKSUM VS EXPECTED" % NBANKS, "", "",
             "PASSES:     0000", "ERRORS:     0000", "LAST BAD:   --", "",
             "0" * NBANKS + " <- ERRORS PER BANK"]
    text = bytearray()
    for l in lines: text += scr(l.ljust(40))
    a.L('text'); a.b += text.ljust(512, b' ')
    a.L('explo'); a.b += bytes(e & 0xFF for e in exp)
    a.L('exphi'); a.b += bytes(e >> 8 for e in exp)
    a.L('cnt'); a.b += bytes(6 + NBANKS)
    return a

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

def build(seed=1):
    rnd = random.Random(seed)
    banks = [[bytearray(rnd.getrandbits(8) for _ in range(8192)) for _ in range(2)] for _ in range(NBANKS)]
    # program size does not depend on the expected values: build twice
    prog = build_program([0]*NBANKS).resolve()
    assert len(prog) <= 0x0800, len(prog)
    romh0 = banks[0][1]
    stub = boot_stub(len(prog))
    romh0[0:len(stub)] = stub
    romh0[0x100:0x100+len(prog)] = prog
    romh0[0x1FFA:0x2000] = struct.pack('<HHH', 0xE000, 0xE000, 0xE000)
    exp = [(sum(b[0]) + sum(b[1])) & 0xFFFF for b in banks]
    # bank 0 holds the expected sums: its sum is fixed with a pad area
    exp[0] = 0x1234
    prog = build_program(exp).resolve()
    romh0[0x100:0x100+len(prog)] = prog
    PAD = 0x1000
    romh0[PAD:PAD+0x200] = bytes(0x200)
    delta = (exp[0] - (sum(banks[0][0]) + sum(romh0))) & 0xFFFF
    i = PAD
    while delta:
        v = min(255, delta); romh0[i] = v; delta -= v; i += 1
    assert i <= PAD + 0x200
    assert [(sum(b[0]) + sum(b[1])) & 0xFFFF for b in banks] == exp
    return banks, prog, exp

def write_crt(path, banks):
    hdr = b'C64 CARTRIDGE   ' + struct.pack('>IHHBB', 0x40, 0x0100, 32, 1, 0) + bytes(6)
    hdr += b'GOCART EF READ TEST'.ljust(32, b'\0')
    out = bytearray(hdr)
    for n, (l, h) in enumerate(banks):
        for addr, d in ((0x8000, l), (0xA000, h)):
            out += b'CHIP' + struct.pack('>IHHHH', 0x10 + len(d), 2, n, addr, len(d)) + d
    open(path, 'wb').write(out)

if __name__ == '__main__':
    banks, prog, exp = build()
    write_crt(sys.argv[1] if len(sys.argv) > 1 else 'ef_read_test.crt', banks)
    print('program %d bytes, expected sums %s' % (len(prog), ' '.join('%04X' % e for e in exp)))
