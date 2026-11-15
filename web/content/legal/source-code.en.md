## What we offer

The LordOS images (the live ISOs for PCs, the virtual machine image and the Raspberry Pi image) contain Debian 13 packages, and on the Raspberry Pi some packages from Raspberry Pi's archive and our own build of the Raspberry Pi Linux kernel. Many of these programs are under the GNU General Public License (GPL), the LGPL or another copyleft license. Those licenses give you the right to the source code of the programs, and we make it available.

**Written offer.** For at least three years after we last publish an image or a package, anyone who has it can get from us the complete corresponding source code of any GPL, LGPL or other copyleft program in it, in machine-readable form, as a download and free of charge (or on a physical medium, for no more than what making and shipping that medium costs). The offer covers every LordOS image and package published since 0.8.0.

## Where the source is

Next to each image on its release page there is a file named like the image with `.sources.tsv` at the end (for example `lordos-live-0.8.8-amd64.sources.tsv`). It lists every source package in that image with its exact version, the binary packages built from it, and a link to that exact source:

| Origin | Where the source is |
|---|---|
| Debian | [snapshot.debian.org](https://snapshot.debian.org), which keeps every version Debian ever published: `snapshot.debian.org/package/<source>/<version>/` |
| Raspberry Pi archive (Raspberry Pi image) | our own copy of the exact source in `lordos.org/apt/pool/rpi/source/<letter>/<source>/` (FFmpeg, kms++, and, in development after LordOS 0.8.8.8 and published with the next update of our package repository, Raspberry Pi's kernel and firmware in the 0.8.8 image); the list links its `.dsc`, and the files it names are next to it. Otherwise Raspberry Pi's archive |
| LordOS | the [LordOS repository](https://github.com/lordbasex/LordOS) at the release tag. The `lord-doc` and `lord-media` helpers link GPL builds of Poppler and FFmpeg; their source is that same repository |
| The Raspberry Pi kernel | Raspberry Pi's [Linux tree](https://github.com/raspberrypi/linux) at the commit the list names, with our configuration in `tools/rpi/build-kernel.sh` (same tag) and the complete `.config` in `/boot/config-<release>` in the image. In development after LordOS 0.8.8.8, and published with the next update of our package repository, we also keep a copy ourselves: the tree at that commit, the exact `.config` and the build script, listed in [lordos.org/apt/pool/rpi/source/l/linux-lordos-rpi-2712/SOURCE](https://lordos.org/apt/pool/rpi/source/l/linux-lordos-rpi-2712/SOURCE) |
| rclone | rclone's own release, unmodified: [github.com/rclone/rclone](https://github.com/rclone/rclone) at that version's tag |

On an installed system, each package's license is in `/usr/share/doc/<package>/copyright`. The details are in [docs/SOURCE-OFFER.md](https://github.com/lordbasex/LordOS/blob/main/docs/SOURCE-OFFER.md).

## How to ask

If a link doesn't work or you want us to send you the source, open a [source code request](https://github.com/lordbasex/LordOS/issues/new?template=source-request.yml) on GitHub. Name the file you got from us (for example `lordos-live-0.8.8-amd64.iso`) and the program or source packages you want, or "all of it". If you don't have a GitHub account, use [our contact form](https://www.iperfex.com/contacto/?lang=en) with the same information. We answer there with a download link.

The images of 0.8.8 and earlier were published before these lists existed; their lists are being made from the published images. Until a list is on its release page, the offer above applies just the same.
