#!/usr/bin/env python3
# vmimport_gen.py — makes snapshot layers of other virtualization apps' disk formats for the importer's tests
# (docs/VIRTUALIZATION.md 6.22): QEMU can read a VDI or an .hds base but not a layer over it, so lord-vm merges them
# itself, and these files check that merge byte for byte. Everything here is invented test data.
#
#   vmimport_gen.py vdi-diff BASE.vdi OUT.vdi OFFSET_MB...   a VDI differencing layer over BASE: the 1 MB blocks at
#                                                           OFFSET_MB hold the byte 0xA0 + their index; prints its uuid
#   vmimport_gen.py vdi-zero BASE.vdi OUT.vdi OFFSET_MB      a layer that marks one block as zeros
#   vmimport_gen.py hds-delta BASE.hds OUT.hds OFFSET_MB...  the same for an .hds data file
#   vmimport_gen.py vdi-uuid FILE.vdi                       the image's own uuid, as the settings file writes it
#   vmimport_gen.py expect RAW OUT OFFSET_MB:BYTE...        the raw disk the merge must give
import struct, sys, uuid

MB = 1 << 20


def rtuuid_str(b):
    # VirtualBox keeps a UUID's first three fields little-endian in memory
    return str(uuid.UUID(bytes_le=bytes(b)))


def vdi_layer(base, out, offsets, zero=False):
    with open(base, 'rb') as f:
        h = bytearray(f.read(512))
    assert struct.unpack_from('<I', h, 64)[0] == 0xbeda107f
    bmap_off, data_off = struct.unpack_from('<II', h, 340)
    block = struct.unpack_from('<I', h, 376)[0]
    blocks = struct.unpack_from('<I', h, 384)[0]
    parent = bytes(h[392:408])
    me = uuid.uuid4()
    struct.pack_into('<I', h, 76, 4)                     # differencing
    h[392:408] = me.bytes_le
    h[424:440] = parent                                  # uuid_link: the parent's image
    bmap = [0xffffffff] * blocks                         # not in this layer
    data = []
    for i, o in enumerate(offsets):
        if zero:
            bmap[o] = 0xfffffffe
        else:
            bmap[o] = len(data)
            data.append(bytes([0xa0 + i]) * block)
    struct.pack_into('<I', h, 388, len(data))
    with open(out, 'wb') as f:
        f.write(h)
        f.seek(bmap_off)
        f.write(struct.pack('<%dI' % blocks, *bmap))
        for k, d in enumerate(data):
            f.seek(data_off + k * block)
            f.write(d)
    print(str(me))


def hds_delta(base, out, offsets):
    with open(base, 'rb') as f:
        h = bytearray(f.read(64))
    magic = bytes(h[:16])
    assert magic in (b'WithoutFreeSpace', b'WithouFreSpacExt')
    tracks, bat = struct.unpack_from('<II', h, 28, )
    cluster = tracks * 512
    mult = tracks if magic == b'WithouFreSpacExt' else 1
    batv = [0] * bat
    first = ((64 + 4 * bat + cluster - 1) // cluster) * cluster     # data after the table, cluster-aligned
    body = []
    for i, o in enumerate(offsets):
        at = first + i * cluster
        batv[o] = at // 512 // mult
        body.append((at, bytes([0xb0 + i]) * cluster))
    struct.pack_into('<I', h, 48, first // 512)                        # data_off
    struct.pack_into('<I', h, 44, 0)                                   # not in use
    with open(out, 'wb') as f:
        f.write(h)
        f.write(struct.pack('<%dI' % bat, *batv))
        for at, d in body:
            f.seek(at)
            f.write(d)


def expect(raw, out, items):
    with open(raw, 'rb') as f:
        d = bytearray(f.read())
    for it in items:
        o, b = it.split(':')
        o = int(o) * MB
        d[o:o + MB] = bytes([int(b, 0)]) * MB
    with open(out, 'wb') as f:
        f.write(d)


cmd = sys.argv[1]
if cmd == 'vdi-diff':
    vdi_layer(sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4:]])
elif cmd == 'vdi-zero':
    vdi_layer(sys.argv[2], sys.argv[3], [int(sys.argv[4])], zero=True)
elif cmd == 'hds-delta':
    hds_delta(sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4:]])
elif cmd == 'vdi-uuid':
    with open(sys.argv[2], 'rb') as f:
        h = f.read(512)
    print(rtuuid_str(h[392:408]))
elif cmd == 'expect':
    expect(sys.argv[2], sys.argv[3], sys.argv[4:])
else:
    sys.exit('usage: see the header')
