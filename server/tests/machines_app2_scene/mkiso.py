# mkiso.py — a tiny ISO 9660 installer disc for tests/machines_app2_headless.sh (a test tool, never shipped): a volume
# label, /EFI/BOOT/<file> and an El Torito catalog with one UEFI entry, enough for lord-vm --iso-detect (vm/vmiso.c).
#   python3 mkiso.py OUT LABEL BOOTFILE      e.g. BOOTAA64.EFI (an arm64 disc), BOOTX64.EFI (amd64)
import struct, sys

S = 2048
out, label, bootfile = sys.argv[1], sys.argv[2], sys.argv[3]
img = bytearray(32 * S)

def rec(name, lba, size, is_dir):
    n = name if isinstance(name, bytes) else name.encode()
    rl = 33 + len(n) + ((33 + len(n)) & 1)
    r = bytearray(rl)
    r[0] = rl
    r[2:6] = struct.pack('<I', lba); r[6:10] = struct.pack('>I', lba)
    r[10:14] = struct.pack('<I', size); r[14:18] = struct.pack('>I', size)
    r[25] = 2 if is_dir else 0
    r[32] = len(n)
    r[33:33 + len(n)] = n
    return bytes(r)

def directory(lba, parent, entries):
    d = rec(b'\0', lba, S, True) + rec(b'\1', parent, S, True) + b''.join(entries)
    img[lba * S:lba * S + len(d)] = d

pvd = bytearray(S)
pvd[0] = 1; pvd[1:6] = b'CD001'; pvd[6] = 1
pvd[40:72] = label.encode()[:32].ljust(32, b' ')
pvd[156:156 + 34] = rec(b'\0', 20, S, True)
img[16 * S:17 * S] = pvd
br = bytearray(S)
br[0] = 0; br[1:6] = b'CD001'; br[6] = 1; br[7:30] = b'EL TORITO SPECIFICATION'; br[0x47:0x4b] = struct.pack('<I', 19)
img[17 * S:18 * S] = br
img[18 * S] = 255; img[18 * S + 1:18 * S + 6] = b'CD001'
cat = bytearray(S)
cat[0] = 1; cat[1] = 0xEF; cat[30] = 0x55; cat[31] = 0xAA
s = sum(struct.unpack('<16H', bytes(cat[:32]))) & 0xffff
cat[28:30] = struct.pack('<H', (0x10000 - s) & 0xffff)
cat[32] = 0x88
img[19 * S:20 * S] = cat
directory(20, 20, [rec('EFI', 21, S, True)])
directory(21, 20, [rec('BOOT', 22, S, True)])
directory(22, 21, [rec(bootfile + ';1', 24, 4, False)])
open(out, 'wb').write(bytes(img))
