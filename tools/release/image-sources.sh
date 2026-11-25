#!/bin/sh
# image-sources.sh — the list of source packages in a LordOS image, published next to the image so the
# corresponding source of every GPL (and other copyleft) binary in it can be found. See
# docs/SOURCE-OFFER.md and docs/COMPLIANCE.md.
#
#   tools/release/image-sources.sh <image or root folder> [out.tsv]
#
# The input is one of:
#   - a root folder (the image's system, what tools/iso/build.sh, build-ova.sh and tools/rpi/build-img.sh
#     pass while they build): read in place; its APT lists, when still there, say where each package came from
#   - a live ISO (lordos-live-<version>-<arch>.iso): live/filesystem.squashfs is read in place inside the ISO
#     with unsquashfs -o (no mount, no copy)
#   - a Raspberry Pi image (lordos-<version>-rpi5-arm64.img.xz, or .img): decompressed into a temporary
#     folder (several GB), then the ext4 system partition is read with debugfs (no mount)
#   - an OVA (lordos-<version>-amd64.ova) or a .vmdk: the disk is converted to a temporary raw file with
#     qemu-img (sparse, a few GB on disk), then read with debugfs
# None of them needs root or a loop device. Needs dpkg-query and python3; unsquashfs (squashfs-tools) for an
# ISO, debugfs (e2fsprogs, in /usr/sbin) and xz for an image, qemu-img (qemu-utils) and tar for an OVA.
# TMPDIR picks where the temporary files go (default /var/tmp).
#
# Default output: <image name without .iso, .img.xz, .ova>.sources.tsv next to the image (for a folder,
# ./sources.tsv). One line per source package and version:
#
#   origin  source  source_version  binaries  source_url  note
#
# origin is debian (the source is on snapshot.debian.org, which keeps every version Debian ever published),
# raspberrypi (Raspberry Pi's archive; LordOS's own copy of the source when tools/release/rpi-mirror.list pins
# it), lordos (built by LordOS from this repository at the release tag; the Raspberry Pi kernel from
# Raspberry Pi's tree at a fixed commit plus tools/rpi/build-kernel.sh, pointing at LordOS's copy of the tree,
# .config and script in lordos.org/apt/pool/rpi/source/ when a "tarball" line pins it; a Debian package LordOS
# rebuilds, version ~lordos<n>+<m> such as WPE WebKit, pointing at its .dsc in lordos.org/apt/pool/main/source/,
# docs/WPEWEBKIT.md) or vendor (an upstream binary
# repackaged by LordOS, with its upstream source). Lines starting with # are comments.
#
# Environment: LORDOS_TAG (the release tag; default v<lordos version>-<channel>, the channel from this
# repository's package.json), KERNEL_SOURCE (the SOURCE file tools/rpi/build-kernel.sh writes next to its
# packages, for the LordOS kernel's exact commit; without it the commits known in docs/RASPBERRY.md are used),
# SOURCE_DATE_EPOCH (the date written in the header; default now).
set -eu
in=${1:?usage: image-sources.sh <image.iso|image.img[.xz]|image.ova|root folder> [out.tsv]}
[ -e "$in" ] || { echo "image-sources.sh: no $in" >&2; exit 1; }
for t in dpkg-query python3; do
    command -v "$t" >/dev/null || { echo "image-sources.sh: $t is needed (run it on Debian)" >&2; exit 1; }
done
PATH=$PATH:/usr/sbin:/sbin
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

base=$(basename "$in")
case "$base" in
    *.img.xz) name=${base%.img.xz} ;;
    *.iso|*.img|*.ova|*.vmdk|*.raw) name=${base%.*} ;;
    *) name="" ;;
esac
if [ -d "$in" ]; then out=${2:-sources.tsv}; else out=${2:-$(dirname "$in")/$name.sources.tsv}; fi

tmp=$(mktemp -d "${TMPDIR:-/var/tmp}/image-sources.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/root/var/lib/dpkg" "$tmp/root/etc/apt/preferences.d"
# the files read from an image: the dpkg database and the Raspberry Pi archive's pins
FILES="var/lib/dpkg/status etc/apt/preferences.d/raspberrypi"

# a disk image: the system partition's offset in bytes (the largest Linux partition, MBR or GPT)
part_offset() {
    python3 -I - "$1" <<'PY'
import struct, sys
LINUX = {"0fc63daf-8483-4772-8e79-3d69d8477de4", "4f68bce3-e8cd-4db1-96e7-fbcaf984b709",
         "b921b045-1df0-41c3-af44-4c6f280d3fae"}   # Linux data, root x86-64 (8304), root arm64
def guid(b):
    a, b2, c = struct.unpack("<IHH", b[:8])
    return "%08x-%04x-%04x-%s-%s" % (a, b2, c, b[8:10].hex(), b[10:16].hex())
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        sys.exit("no partition table")
    parts = []
    for i in range(4):
        e = mbr[446 + 16 * i:462 + 16 * i]
        ptype, start, count = e[4], *struct.unpack("<II", e[8:16])
        if ptype == 0xEE:                               # protective MBR: GPT
            f.seek(512)
            h = f.read(92)
            if h[:8] != b"EFI PART":
                sys.exit("protective MBR without a GPT header")
            lba, n, size = struct.unpack("<QII", h[72:88])
            f.seek(lba * 512)
            for j in range(n):
                g = f.read(size)
                if guid(g[:16]) in LINUX:
                    first, last = struct.unpack("<QQ", g[32:48])
                    parts.append((last - first + 1, first))
            break
        if ptype == 0x83 and count:
            parts.append((count, start))
if not parts:
    sys.exit("no Linux partition")
print(max(parts)[1] * 512)
PY
}

# a live ISO: the byte offset of live/filesystem.squashfs (ISO 9660, with Rock Ridge names when present)
iso_offset() {
    python3 -I - "$1" "${ISO_SQUASHFS:-live/filesystem.squashfs}" <<'PY'
import struct, sys
path = [p.lower() for p in sys.argv[2].split("/") if p]
f = open(sys.argv[1], "rb")
def read(lba, n):
    f.seek(lba * 2048)
    return f.read(n)
pvd = read(16, 2048)
if pvd[1:6] != b"CD001" or pvd[0] != 1:
    sys.exit("not an ISO 9660 image")
def entries(rec):
    lba, size = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0]
    data = read(lba, size)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:                                     # records don't cross sectors: next sector
            i = (i // 2048 + 1) * 2048
            continue
        r = data[i:i + n]
        nl = r[32]
        ident = r[33:33 + nl]
        name = ident.decode("latin-1").split(";")[0].rstrip(".").lower()
        su = r[33 + nl + (0 if nl % 2 else 1):]        # System Use area: Rock Ridge's NM is the real name
        j = 0
        while j + 4 <= len(su):
            sig, ln = su[j:j + 2], su[j + 2]
            if ln < 4:
                break
            if sig == b"NM":
                name = su[j + 5:j + ln].decode("utf-8", "replace").lower()
            j += ln
        if ident not in (b"\x00", b"\x01"):
            yield name, r
        i += n
rec = pvd[156:190]
for k, part in enumerate(path):
    for name, r in entries(rec):
        if name == part:
            rec = r
            break
    else:
        sys.exit("no " + "/".join(path[:k + 1]) + " in the ISO")
print(struct.unpack("<I", rec[2:6])[0] * 2048)
PY
}

# debugfs on a raw disk at an offset: copies FILES into $tmp/root
from_ext4() {
    for f in $FILES; do
        debugfs -R "cat /$f" "$1?offset=$2" > "$tmp/root/$f" 2> "$tmp/debugfs.log" || true
        [ -s "$tmp/root/$f" ] || rm -f "$tmp/root/$f"
    done
}

case "$in" in
    *.iso)
        command -v unsquashfs >/dev/null || { echo "image-sources.sh: unsquashfs is needed (squashfs-tools)" >&2; exit 1; }
        off=$(iso_offset "$in")
        for f in $FILES; do
            unsquashfs -o "$off" -cat "$in" "$f" > "$tmp/root/$f" 2> /dev/null || true
            [ -s "$tmp/root/$f" ] || rm -f "$tmp/root/$f"
        done
        R="$tmp/root" ;;
    *.img.xz|*.img|*.raw|*.ova|*.vmdk)
        command -v debugfs >/dev/null || { echo "image-sources.sh: debugfs is needed (e2fsprogs)" >&2; exit 1; }
        disk="$tmp/disk.raw"
        case "$in" in
            *.xz) command -v xz >/dev/null || { echo "image-sources.sh: xz is needed" >&2; exit 1; }
                  echo "decompressing $base into $tmp (several GB)" >&2
                  xz -dc "$in" > "$disk" ;;
            *.ova|*.vmdk)
                  command -v qemu-img >/dev/null || { echo "image-sources.sh: qemu-img is needed (qemu-utils)" >&2; exit 1; }
                  vmdk=$in
                  if [ "${in%.ova}" != "$in" ]; then
                      vm=$(tar -tf "$in" | grep '\.vmdk$' | head -n 1)
                      [ -n "$vm" ] || { echo "image-sources.sh: no .vmdk in $base" >&2; exit 1; }
                      tar -xf "$in" -C "$tmp" "$vm"
                      vmdk="$tmp/$vm"
                  fi
                  echo "converting the disk of $base into $tmp (sparse)" >&2
                  qemu-img convert -O raw "$vmdk" "$disk"
                  rm -f "$tmp"/*.vmdk ;;
            *) disk=$in ;;
        esac
        from_ext4 "$disk" "$(part_offset "$disk")"
        [ "$disk" = "$in" ] || rm -f "$disk"
        R="$tmp/root" ;;
    *)
        [ -d "$in" ] || { echo "image-sources.sh: $in is not an .iso, .img[.xz], .ova, .vmdk or a folder" >&2; exit 1; }
        R=$in ;;
esac
[ -s "$R/var/lib/dpkg/status" ] || { echo "image-sources.sh: no dpkg database (var/lib/dpkg/status) in $in" >&2; exit 1; }
mkdir -p "$tmp/admin"
cp "$R/var/lib/dpkg/status" "$tmp/admin/status"
dpkg-query --admindir="$tmp/admin" -W \
    -f='${db:Status-Abbrev}\t${source:Package}\t${source:Version}\t${Package}\t${Version}\t${Architecture}\t${Maintainer}\n' \
    > "$tmp/packages.tsv"

channel=$(sed -n 's/.*"channel": *"\([^"]*\)".*/\1/p' "$repo/package.json" 2>/dev/null | head -n 1)
IS_ROOT=$R IS_TMP=$tmp IS_OUT=$out IS_INPUT=$base IS_REPO=$repo IS_CHANNEL=$channel \
IS_TAG=${LORDOS_TAG:-} IS_KSRC=${KERNEL_SOURCE:-} python3 -I - <<'PY'
import datetime, fnmatch, glob, hashlib, os, re, sys, urllib.parse

E = os.environ
root, tmp, out = E["IS_ROOT"], E["IS_TMP"], E["IS_OUT"]
REPO = "https://github.com/lordbasex/LordOS"
SNAPSHOT = "https://snapshot.debian.org/package/{}/{}/"
RPI_POOL = "https://archive.raspberrypi.com/debian/pool/main/{}/{}/"
# LordOS's copy: the pool has no folder listings, so the link is a file in it (the .dsc, which `dget` takes,
# or the kernel's SOURCE, which lists its files); "+" must be %2B there (the storage reads "+" as a space)
RPI_MIRROR = "https://lordos.org/apt/pool/rpi/source/{}/{}/{}"
# Debian packages LordOS rebuilds (version ~lordos13+N: WPE WebKit, tools/pkg/build-wpewebkit.sh, docs/WPEWEBKIT.md):
# the exact source package is published next to the binaries, the .dsc in pool/main/source/<prefix>/<source>/
LORDOS_SRC = "https://lordos.org/apt/pool/main/source/{}/{}/{}"
# the LordOS Raspberry Pi kernel's source commit by upstream version, when no SOURCE file is given
# (docs/RASPBERRY.md, "The LordOS kernel": every revision of 6.18.57 is commit 2341883150378...)
KNOWN_KERNELS = {"6.18.57": ("https://github.com/raspberrypi/linux.git", "rpi-6.18.y",
                             "234188315037848e5b07d65b85d38edc64095caa")}

def q(v):
    return urllib.parse.quote(v, safe="")

def read(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""

pkgs = []
for line in read(os.path.join(tmp, "packages.tsv")).splitlines():
    st, src, sver, name, ver, arch, maint = (line.split("\t") + [""] * 7)[:7]
    if st.startswith("ii"):
        pkgs.append(dict(src=src, sver=sver, name=name, ver=ver, arch=arch, maint=maint))
if not pkgs:
    sys.exit("image-sources.sh: no installed packages in the dpkg database")

# where each package came from: the APT lists when the build left them (exact), else the pins
origin_of = {}                                       # (name, version) -> "raspberrypi" | "debian"
for lst in glob.glob(os.path.join(root, "var/lib/apt/lists/*_Packages")):
    org = "raspberrypi" if "raspberrypi" in os.path.basename(lst) else "debian"
    name = ver = None
    for line in read(lst).splitlines() + [""]:
        if line.startswith("Package: "):
            name = line[9:].strip()
        elif line.startswith("Version: "):
            ver = line[9:].strip()
        elif not line and name:
            if ver and (origin_of.get((name, ver)) != "raspberrypi"):
                origin_of[(name, ver)] = org
            name = ver = None
have_lists = bool(origin_of)
rpi_globs = []                                       # Package globs pinned to Raspberry Pi's archive above Debian
for stanza in re.split(r"\n\s*\n", read(os.path.join(root, "etc/apt/preferences.d/raspberrypi"))):
    fields = dict((k.strip(), v.strip()) for k, _, v in
                  (l.partition(":") for l in stanza.splitlines() if ":" in l and not l.startswith("#")))
    try:
        prio = int(fields.get("Pin-Priority", "0"))
    except ValueError:
        prio = 0
    if "raspberrypi" in fields.get("Pin", "") and prio > 500:
        rpi_globs += fields.get("Package", "").split()
is_pi = bool(rpi_globs) or os.path.exists(os.path.join(root, "etc/apt/sources.list.d/raspberrypi.list"))

# the Raspberry Pi sources LordOS keeps a copy of (tools/release/rpi-mirror.list, architecture "source" or
# "source-pool"), and the LordOS kernels whose source it keeps ("tarball": the tree, .config and build script)
mirrored, kept_kernels = set(), set()
for line in read(os.path.join(E["IS_REPO"], "tools/release/rpi-mirror.list")).splitlines():
    f = line.split()
    if len(f) >= 3 and not line.startswith("#"):
        if f[2] in ("source", "source-pool"):
            mirrored.add((f[0], f[1]))
        elif f[2] == "tarball":
            kept_kernels.add((f[0], f[1]))

lordos_ver = next((p["ver"] for p in pkgs if p["name"] == "lordos"), "")
tag = E.get("IS_TAG") or (("v" + lordos_ver + ("-" + E["IS_CHANNEL"] if E.get("IS_CHANNEL") else "")) if lordos_ver else "")
tag_url = f"{REPO}/tree/{tag}" if tag else REPO

ksrc = dict(l.split("=", 1) for l in read(E["IS_KSRC"]).splitlines() if "=" in l) if E.get("IS_KSRC") else {}

warnings = []
def classify(p):
    """(origin, source_url, note) for one binary package"""
    src, sver, name = p["src"], p["sver"], p["name"]
    if src.startswith("linux-lordos-") or re.match(r"linux-(image|headers)-.*-lordos", name):
        upstream = sver.split("-")[0]
        rel = re.sub(r"^linux-(image|headers)-", "", name)
        repo_url, branch, commit = ksrc.get("source"), ksrc.get("branch"), ksrc.get("commit")
        if not commit:
            repo_url, branch, commit = KNOWN_KERNELS.get(upstream, (None, None, None))
        if commit:
            url = f"{(repo_url or '').removesuffix('.git')}/tree/{commit}"
        else:
            url = "https://github.com/raspberrypi/linux"
            warnings.append(f"kernel {name}: source commit unknown (pass KERNEL_SOURCE=<work>/out/SOURCE)")
        kept = (src, sver) in kept_kernels
        if kept:
            url = RPI_MIRROR.format(src[0], q(src), "SOURCE")
        return ("lordos", url,
                f"Linux {upstream} from Raspberry Pi's tree ({branch or 'rpi-' + '.'.join(upstream.split('.')[:2]) + '.y'}"
                f"{', commit ' + commit if commit else ''}); configured and built by tools/rpi/build-kernel.sh "
                f"at {tag_url}; the complete configuration is /boot/config-{rel} in the image"
                + ("; LordOS keeps this source: the tree at that commit, the .config and the build script" if kept else ""))
    if re.search(r"~lordos\d+\+\d+$", sver) and not src.startswith("lordos"):
        prefix = src[:4] if src.startswith("lib") and len(src) > 3 else src[0]
        return ("lordos", LORDOS_SRC.format(prefix, q(src), q(f"{src}_{sver.split(':', 1)[-1]}.dsc")),
                "Debian's source package rebuilt for Debian 13 by LordOS (tools/pkg/, docs/WPEWEBKIT.md); this exact source "
                "(the .dsc and the files it lists, in the same folder) is kept by LordOS; the license is "
                "/usr/share/doc/<package>/copyright in the image")
    if p["maint"].startswith("LordOS ") or name.startswith("lordos"):
        if name == "lordos-rclone":
            up = re.sub(r"-[^-]*$", "", p["ver"])
            return ("vendor", f"https://github.com/rclone/rclone/tree/v{up}",
                    f"rclone's own release binary v{up}, unmodified (MIT); packaged by tools/iso/mkdeb-rclone.sh at {tag_url}")
        note = "LordOS (MIT) at the release tag"
        if name == "lordos":
            note += ("; lord-doc links Poppler and lord-media Debian's FFmpeg, both GPL-2.0-or-later as built "
                     "by Debian: their complete source is this tree (THIRD_PARTY.md)")
        return ("lordos", tag_url, note)
    org = origin_of.get((name, p["ver"]))
    if org is None:
        if is_pi and (any(fnmatch.fnmatchcase(name, g) for g in rpi_globs) or re.search(r"\+rp[it]\d", p["ver"])):
            org = "raspberrypi"
        else:
            org = "debian"
            if have_lists:
                warnings.append(f"{name} {p['ver']}: in no APT list of the build; listed as Debian")
    if org == "raspberrypi":
        if (src, sver) in mirrored:
            return ("raspberrypi", RPI_MIRROR.format(src[0], q(src), q(f"{src}_{sver.split(':', 1)[-1]}.dsc")),
                    "Raspberry Pi's archive; this exact source is kept by LordOS (the .dsc and the files it lists, in the same folder)")
        prefix = src[:4] if src.startswith("lib") and len(src) > 3 else src[0]
        return ("raspberrypi", RPI_POOL.format(prefix, q(src)), "Raspberry Pi's archive")
    return ("debian", SNAPSHOT.format(q(src), q(sver)), "")

groups = {}
for p in pkgs:
    org, url, note = classify(p)
    key = (org, p["src"], p["sver"])
    g = groups.setdefault(key, dict(url=url, note=note, bins=[]))
    g["bins"].append(f"{p['name']}={p['ver']}")
order = {"debian": 0, "raspberrypi": 1, "lordos": 2, "vendor": 3}
rows = sorted(groups.items(), key=lambda kv: (order.get(kv[0][0], 9), kv[0][1], kv[0][2]))
count = {}
for (org, _, _), _ in rows:
    count[org] = count.get(org, 0) + 1

sde = E.get("SOURCE_DATE_EPOCH", "")
when = datetime.datetime.fromtimestamp(int(sde), datetime.timezone.utc) if sde.isdigit() else datetime.datetime.now(datetime.timezone.utc)
now = when.strftime("%Y-%m-%dT%H:%M:%SZ")
status_sha = hashlib.sha256(open(os.path.join(tmp, "admin/status"), "rb").read()).hexdigest()
lines = [
    f"# Source packages of {E['IS_INPUT']} (LordOS {lordos_ver or '?'}), generated {now} by tools/release/image-sources.sh",
    f"# from its dpkg database (var/lib/dpkg/status, SHA-256 {status_sha}): {len(pkgs)} binary packages,",
    "# " + ", ".join(f"{count[o]} {o}" for o in sorted(count, key=lambda o: order.get(o, 9))) + " source packages.",
    "# The written offer for this source, valid for three years: https://lordos.org/legal/source-code/",
    "# (docs/SOURCE-OFFER.md). Debian's sources: snapshot.debian.org keeps every version; each package's",
    "# license is /usr/share/doc/<package>/copyright in the image.",
    "origin\tsource\tsource_version\tbinaries\tsource_url\tnote",
]
for (org, src, sver), g in rows:
    lines.append("\t".join([org, src, sver, ",".join(sorted(g["bins"])), g["url"], g["note"]]))
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with open(out, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print(f"wrote {out}: {len(pkgs)} binary packages from {len(rows)} source packages ("
      + ", ".join(f"{o} {count[o]}" for o in sorted(count, key=lambda o: order.get(o, 9))) + ")")
not_kept = sorted(f"{s} {v}" for (o, s, v), _ in rows if o == "raspberrypi" and (s, v) not in mirrored)
if not_kept:
    print("note: Raspberry Pi sources LordOS keeps no copy of (pin the copyleft ones with architecture 'source' "
          "in tools/release/rpi-mirror.list, or 'source-pool' once Raspberry Pi's index has dropped them): "
          + ", ".join(not_kept))
for (o, s, v), _ in rows:
    if o == "lordos" and s.startswith("linux-lordos-") and (s, v) not in kept_kernels:
        warnings.append(f"kernel {s} {v}: LordOS keeps no copy of its source (a 'tarball' line in "
                        "tools/release/rpi-mirror.list and tools/rpi/kernel/<name>_<version>/, docs/RELEASING.md)")
for w in warnings:
    print("warning: " + w, file=sys.stderr)
PY
