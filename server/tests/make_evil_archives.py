#!/usr/bin/env python3
"""Builds malicious archives for tests/doc_evil.sh (axia-doc safe extraction).

Usage: python3 -I make_evil_archives.py <folder>
Each one tries to write outside the destination folder in a different way. The targets
point to <folder>/../FUERA-* (a name the test checks afterwards)."""
import io, os, sys, tarfile, zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def tar(name, members, mode='w'):
    with tarfile.open(os.path.join(out, name), mode) as t:
        for m, data in members:
            t.addfile(m, io.BytesIO(data) if data is not None else None)

def reg(path, data=b'x', mode=0o644):
    i = tarfile.TarInfo(path); i.size = len(data); i.mode = mode; return (i, data)

def sym(path, target):
    i = tarfile.TarInfo(path); i.type = tarfile.SYMTYPE; i.linkname = target; return (i, None)

def hard(path, target):
    i = tarfile.TarInfo(path); i.type = tarfile.LNKTYPE; i.linkname = target; return (i, None)

def dev(path):
    i = tarfile.TarInfo(path); i.type = tarfile.CHRTYPE; i.devmajor = 1; i.devminor = 3; return (i, None)

# 1. "zip slip": ../ in the name
with zipfile.ZipFile(os.path.join(out, 'slip.zip'), 'w') as z:
    z.writestr('ok.txt', 'bien')
    z.writestr('../FUERA-slip.txt', 'mal')
    z.writestr('a/../../FUERA-slip2.txt', 'mal')
# 2. absolute path
tar('absoluta.tar', [reg('ok.txt'), reg('/tmp/FUERA-abs.txt')])
# 3. link to the outside and then a file "through" the link
tar('enlace.tar', [sym('salida', '..'), reg('salida/FUERA-link.txt'), sym('etc', '/etc'), reg('etc/FUERA-etc.txt'), reg('ok.txt')])
# 4. chain of links: each looks inside, together they escape
tar('cadena.tar.gz', [reg('d/ok.txt'), sym('d/l1', '..'), sym('d/l2', 'l1/..'), reg('d/l2/FUERA-chain.txt')], 'w:gz')
# 5. hard link to a file outside
tar('duro.tar', [hard('passwd', '/etc/passwd'), hard('p2', '../FUERA-hard'), reg('ok.txt')])
# 6. device, setuid and FIFO
fifo = tarfile.TarInfo('fifo'); fifo.type = tarfile.FIFOTYPE
tar('especiales.tar.xz', [dev('null'), reg('suid', b'#!/bin/sh\n', 0o4755), (fifo, None), reg('ok.txt')], 'w:xz')
# 7. the same file twice (must not overwrite)
tar('repetido.tar', [reg('mismo.txt', b'uno'), reg('mismo.txt', b'dos')])
# 8. healthy archive with folders and names in several scripts
with zipfile.ZipFile(os.path.join(out, 'sano.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('carpeta/ñandú.txt', 'hola')
    z.writestr('carpeta/日本語.txt', 'こんにちは')
    z.writestr('carpeta/sub/vacío/', '')
    z.writestr('README', 'x' * 100000)
# 9. not an archive
open(os.path.join(out, 'basura.zip'), 'wb').write(os.urandom(4096))
# 10. standalone .gz (raw format)
import gzip
with gzip.open(os.path.join(out, 'suelto.txt.gz'), 'wb') as g:
    g.write(b'texto comprimido suelto\n')
print('done:', sorted(os.listdir(out)))
