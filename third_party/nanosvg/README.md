# nanosvg

SVG parser and rasterizer by Mikko Mononen, two C headers.

- Source: https://github.com/memononen/nanosvg
- Commit: `239e102ec2c691f2902e20ace2ed36ee4a35cfe6`
- License: zlib (`LICENSE.txt`)
- Approved by the maintainer on 2026-11-10 for the SVG icons of Linux apps (`docs/MEDIA.md`, "SVG icons").

Files copied unmodified from the repository's `src/` folder and root:

| File | SHA-256 |
|---|---|
| `nanosvg.h` | `e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0` |
| `nanosvgrast.h` | `79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8` |
| `LICENSE.txt` | as published |

**Only `lord-media` compiles it** (`server/src/media/helper/lm_svg.c`, the one file that defines
`NANOSVG_IMPLEMENTATION`), inside its sandbox, after `sandbox_apply_helper()`, on a file descriptor
lord-server opened. Never `lord-server`. `lm_svg.c` bounds what reaches the parser and the rasterizer
(1 MB, elements, nesting, shapes, points, finite coordinates, stroke widths and dashes) and lord-server
kills a job that takes more than 3 s. `build/svgrast-test` (`make -C server test`) and
`build/media-test svg` (`make -C server test-full`) run hostile files through it.

To update: copy the two headers and the license from a newer commit, update the commit and the hashes
here, and run both tests.
