#!/usr/bin/env python3

# List the hardware type of every .crt file in a directory tree, grouped by
# type. Useful to find test titles for the cartridge types of GOCart.64.
#
# usage: crt_scan.py <directory> [type ...]
#   type: only list these hardware types (e.g. 15 43 57)
import os, struct, sys
from collections import defaultdict

TYPES = {
    0: 'Normal', 1: 'Action Replay', 2: 'KCS Power', 3: 'Final Cartridge III',
    4: "Simons' BASIC", 5: 'Ocean', 6: 'Expert', 7: 'Fun Play', 8: 'Super Games',
    9: 'Atomic Power', 10: 'Epyx FastLoad', 11: 'Westermann', 12: 'Rex Utility',
    13: 'Final Cartridge I', 14: 'Magic Formel', 15: 'C64 Game System',
    16: 'Warp Speed', 17: 'Dinamic', 18: 'Zaxxon', 19: 'Magic Desk',
    20: 'Super Snapshot 5', 21: 'Comal-80', 32: 'EasyFlash', 36: 'Retro Replay',
    43: 'Prophet64', 53: 'Pagefox', 57: 'RGCD/Hucky', 60: 'GMod2',
}

# types supported by GOCart.64
SUPPORTED = {0, 4, 5, 7, 8, 11, 12, 13, 15, 16, 17, 18, 19, 21, 32, 43, 57}

def crt_info(path):
    with open(path, 'rb') as f:
        h = f.read(0x40)
        if len(h) < 0x40 or h[:16] != b'C64 CARTRIDGE   ':
            return None
        hlen, ver, typ, exrom, game = struct.unpack('>IHHBB', h[0x10:0x1A])
        sub = h[0x1A] if ver >= 0x0101 else 0
        size = os.path.getsize(path)
    return typ, sub, size

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ or 'usage: crt_scan.py <directory> [type ...]')
    only = {int(t) for t in sys.argv[2:]}
    found = defaultdict(list)
    for root, _, files in os.walk(sys.argv[1]):
        for name in files:
            if name.lower().endswith('.crt'):
                path = os.path.join(root, name)
                try:
                    info = crt_info(path)
                except OSError:
                    continue
                if info and (not only or info[0] in only):
                    found[info[0]].append((path, info[1], info[2]))
    for typ in sorted(found):
        mark = '' if typ in SUPPORTED else '  (not supported)'
        print('== type %d: %s, %d files%s' % (typ, TYPES.get(typ, '?'), len(found[typ]), mark))
        for path, sub, size in sorted(found[typ]):
            print('   %s  (%dK%s)' % (path, size // 1024, ', subtype %d' % sub if sub else ''))

if __name__ == '__main__':
    main()
