# Source code offer

The LordOS images (the live ISOs for amd64 and arm64, the OVA and the Raspberry Pi image) carry Debian 13
binary packages, the Raspberry Pi archive's kernel, firmware and FFmpeg on the Pi, LordOS's own build of the
Raspberry Pi kernel, and LordOS itself. Many of those programs are under the GNU GPL, the LGPL or another
copyleft licence, which ask whoever distributes the binaries to make the corresponding source available. This
page says what LordOS offers, where it is, and how to ask for it. The website version is
[lordos.org/legal/source-code](https://lordos.org/legal/source-code/); the licence inventory is
[COMPLIANCE.md](COMPLIANCE.md).

## The list next to each image

Every image release carries, next to the image, `<image>.sources.tsv`: one line per source package and
version in that image, made from the image's own dpkg database by `tools/release/image-sources.sh` (the image
builders run it; it can also be run on a published file, below). The columns are:

| Column | What |
|---|---|
| `origin` | `debian`, `raspberrypi`, `lordos` or `vendor` |
| `source`, `source_version` | the source package and its exact version (`${source:Package}` and `${source:Version}` of each installed binary package) |
| `binaries` | the binary packages in the image built from it, `name=version` |
| `source_url` | where that exact source is |
| `note` | what else applies (how the LordOS kernel was configured, which LordOS binaries link GPL libraries) |

Where the source is, by origin:

- **`debian`**: [snapshot.debian.org](https://snapshot.debian.org), which keeps every source package Debian ever
  published, at `https://snapshot.debian.org/package/<source>/<version>/` (the `.dsc` and its files; `dget` takes
  the `.dsc`'s URL). These are the very sources Debian built the binaries from: LordOS doesn't rebuild or change
  Debian's packages.
- **`raspberrypi`** (the Pi image only): Raspberry Pi's archive,
  `https://archive.raspberrypi.com/debian/pool/main/<letter>/<source>/`. For the copyleft ones whose binaries
  LordOS ships, LordOS keeps its own copy of the exact source in `https://lordos.org/apt/pool/rpi/source/`, pinned by
  SHA-256 in `tools/release/rpi-mirror.list` (FFmpeg since 0.8.7, kms++ since 0.8.4.1); the list then points there.
  `image-sources.sh` names the Raspberry Pi sources LordOS doesn't keep, so a new copyleft one gets its `source`
  line before the release.
- **`lordos`**: this repository at the release tag (`https://github.com/lordbasex/LordOS/tree/v<version>-beta`):
  `lordos`, `lordos-hwdata` and the other LordOS packages are built from it. `lord-doc` links Poppler and
  `lord-media` Debian's FFmpeg, both GPL-2.0-or-later as Debian builds them, so those two binaries as
  distributed are covered by the GPL; their complete source is the same tree, under the MIT licence, which is
  compatible with it ([THIRD_PARTY.md](../THIRD_PARTY.md)). **The Raspberry Pi kernel**
  (`linux-image-*-lordos*-rpi-*`, source `linux-lordos-rpi-<flavour>`) is GPL-2.0: its source is Raspberry Pi's
  tree at a fixed commit, the configuration and the build are `tools/rpi/build-kernel.sh` at the release tag, and
  the complete `.config` is `/boot/config-<release>` in the image. The 6.18.57 kernels (`-1+lordos` to
  `-3+lordos`, 0.8.0 to today) are `github.com/raspberrypi/linux` commit
  `234188315037848e5b07d65b85d38edc64095caa` ([RASPBERRY.md](RASPBERRY.md), "The LordOS kernel");
  `build-kernel.sh` writes the commit of each build to `SOURCE` next to its packages, and `build-img.sh` passes it
  on.
- **`vendor`**: `lordos-rclone` is rclone's own signed release binary, unmodified (MIT): its source is
  `https://github.com/rclone/rclone` at that version's tag.

## The written offer

For at least **three years** after LordOS last publishes an image or a package, anyone who has it can ask for
the complete corresponding source of any GPL, LGPL or other copyleft program in it, in machine-readable form. We
send it as a download, free of charge (or on a physical medium, for no more than what making and shipping that
medium costs). The offer covers every LordOS image and package published since 0.8.0, including the ones
published before these lists existed.

**How to ask:** open a [source code request](https://github.com/lordbasex/LordOS/issues/new?template=source-request.yml)
on GitHub, naming the file (for example `lordos-live-0.8.8-amd64.iso`) and the program or source packages, or
"all of it". Without a GitHub account, use the [iPERFEX contact form](https://www.iperfex.com/contacto/?lang=en)
with the same information. Answers go to the issue (or the form's reply), never to a personal address.

## Images published before the lists (0.8.8 and earlier)

The 0.8.8 images (`lordos-live-0.8.8-amd64.iso`, `lordos-live-0.8.8-arm64.iso`, `lordos-0.8.8-amd64.ova`,
`lordos-0.8.8-rpi5-arm64.img.xz`, release `v0.8.8-beta`, 2026-11-03) were built before `image-sources.sh`. Their
exact package versions can't be recovered from the repository: the builders install whatever Debian's mirror
served that day, with no snapshot date. They are in each image's dpkg database, so the list is made from the
published file itself, with no root and no mount, on a Debian machine:

```sh
tools/release/image-sources.sh lordos-live-0.8.8-amd64.iso            # reads live/filesystem.squashfs in place
tools/release/image-sources.sh lordos-live-0.8.8-arm64.iso
tools/release/image-sources.sh lordos-0.8.8-rpi5-arm64.img.xz         # decompresses into $TMPDIR (several GB)
tools/release/image-sources.sh lordos-0.8.8-amd64.ova                 # converts the disk into $TMPDIR (sparse)
```

It needs `dpkg-query` and `python3`; `unsquashfs` (squashfs-tools) for an ISO, `debugfs` (e2fsprogs) and `xz`
for the Pi image, `qemu-img` (qemu-utils) and `tar` for the OVA. Each writes `<image>.sources.tsv` next to the
image; [RELEASING.md](RELEASING.md), "Source code", says how they are added to the release. The OVA's list is the
amd64 ISO's without `live-boot` and `live-boot-initramfs-tools`, which `build-ova.sh` removes. Without a newer
`KERNEL_SOURCE` file, the Pi image's kernel line uses the commit above.
