## LordOS: the MIT license

LordOS's own code (the desktop environment, the display server and Wayland compositor, the native apps, the SDK, the themes, the tools and this website) is released under the **MIT license**:

> Copyright (c) 2026 iPERFEX and the LordOS contributors.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, to deal in the software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense and/or sell copies of it, subject to including the copyright notice and the permission notice in all copies or substantial portions of the software. The software is provided "as is", without warranty of any kind.

This is a summary; the [full text of the license](https://github.com/lordbasex/LordOS/blob/main/LICENSE) is the one that applies.

The MIT license covers the code. It doesn't grant rights to the LordOS name, the crown logo or Lumo, the mascot: see the [Trademark and brand policy](/legal/trademark/).

## Third-party components bundled with LordOS

These components live in the LordOS repository (`third_party/` and `assets/`) and keep their own licenses. The complete list is in [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md).

| Component | Used for | License |
|---|---|---|
| QuickJS | JavaScript engine for the interface | MIT |
| stb_image, stb_image_write, stb_truetype | image decoding and encoding, font rasterization | MIT or public domain |
| Monocypher 4 | cryptography for the password vault and keyring | CC0-1.0 or BSD-2-Clause |
| EFF large wordlist | passphrase generator | CC BY 3.0 |
| SecLists common passwords | weak-password check | MIT |
| React, react-reconciler, scheduler | the interface's component model (bundled at build time) | MIT |

## Fonts and data

| Data | Used for | License |
|---|---|---|
| Plus Jakarta Sans 2.7.1 | the LordOS wordmark, in the interface, the logo files and this website | SIL Open Font License 1.1 ([license text](/licenses/PlusJakartaSans-OFL.txt)) |
| Inter | interface text | SIL Open Font License 1.1 |
| JetBrains Mono | terminal and code text | SIL Open Font License 1.1 |
| DB-IP Lite Country | offline IP-to-country lookup in Monitor | CC BY 4.0, IP geolocation by [DB-IP](https://db-ip.com) |
| Natural Earth | the world map in Monitor | public domain |
| Unicode Character Database and CLDR | text layout, bidirectional text, emoji names | Unicode License v3 |

## Debian packages

LordOS is built on Debian. The base system, the Linux kernel, Mesa, PipeWire, CUPS, BlueZ, the firmware, the fonts, the preinstalled browsers and every package installed from the Store are Debian packages (and, on the Raspberry Pi, the kernel and firmware come from the Raspberry Pi archive). Each one is distributed under its own license, mostly free software licenses such as the GPL, LGPL, MIT, BSD and Apache licenses; some firmware is non-free and redistributable.

On an installed system, every package's copyright and license is in `/usr/share/doc/<package>/copyright`. The source code of Debian packages is available from Debian ([sources.debian.org](https://sources.debian.org)).

LordOS links some system libraries dynamically: libwayland and libxkbcommon (MIT), Mesa (MIT), OpenSSL 3 (Apache-2.0), libcrypt (LGPL-2.1+), Poppler, cairo, FreeType and fontconfig (GPL-2.0-or-later, LGPL, FTL), libarchive (BSD-2-Clause) and FFmpeg and libva (LGPL-2.1+; Debian's FFmpeg build is GPL-2.0-or-later). The `lord-doc` and `lord-media` helpers link Poppler and Debian's FFmpeg, so those two binaries, as distributed, are covered by the GPL; the MIT-licensed LordOS source they are built from is compatible with it.

Since 0.8.7 `lordos` also depends on FreeRDP (Apache-2.0), rclone (MIT), fuse3 (LGPL-2.1, GPL-2.0) and the OpenSSH client (BSD). The Store's Disk Formats item, not preinstalled, brings ntfs-3g and exfatprogs (GPL-2.0), dosfstools (GPL-3.0) and, on amd64, apfs-dkms (GPL-2.0). Moving wallpapers are public domain, CC0, CC BY and CC BY-SA, with credits in Settings and in their repository.

## Source code

The source code of LordOS is in [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). If you need the corresponding source of a GPL component we distribute and can't find it, write to us through [our contact form](https://www.iperfex.com/contacto/?lang=en).
