#!/bin/sh
# copyright.sh <source-tree> — appends to <source-tree>/debian/copyright the stanzas of the libwebrtc component
# (libwebrtc/, WebKit's Source/ThirdParty/libwebrtc; docs/WPEWEBKIT.md "LibWebRTC"). Run by tools/pkg/wpewebkit-chroot.sh
# in the build chroot. Each licence text is copied from the component's own licence file, so it is exact; a file that
# moved stops the build. The stanzas come after Debian's, so they win for libwebrtc/ (in copyright-format 1.0 the last
# matching Files stanza applies).
set -eu
T=${1:?usage: copyright.sh <source-tree>}
L=$T/libwebrtc
C=$T/debian/copyright
die() { echo "copyright.sh: $*" >&2; exit 1; }
[ -d "$L/Source/webrtc" ] || die "no libwebrtc component in $T"

# text <file>: a licence file as a copyright-format text block (one space in front, "." for an empty line)
text() {
    [ -s "$1" ] || die "missing licence file ${1#$T/}"
    sed -e 's/[[:space:]]*$//' -e 's/^$/./' -e 's/^/ /' "$1" | sed -e :a -e '/^ \.$/{$d;N;ba' -e '}' | sed '/^ \.$/N;/\n \.$/!P;D'
}
# stanza <files> <licence-name> <licence-file or -> <copyright lines (| between)> [comment]
stanza() {
    printf '\nFiles: %s\n' "$1"
    printf 'Copyright: %s\n' "$(echo "$4" | sed 's/|/\n           /g')"
    printf 'License: %s\n' "$2"
    [ "$3" = - ] || text "$3"
    [ -z "${5:-}" ] || printf 'Comment:\n%s\n' "$(echo "$5" | fold -s -w 110 | sed 's/[[:space:]]*$//; s/^/ /')"
}
S=$L/Source/third_party
{
    stanza 'libwebrtc/*' BSD-3-clause-webrtc "$L/Source/webrtc/LICENSE" \
        '2011-2026 The WebRTC project authors|2017-2026 Apple Inc.' \
        "WebKit's copy of Google's libwebrtc (WebKit's Source/ThirdParty/libwebrtc, at the commit of the release tag; LordOS adds it as the libwebrtc component of the source package). Google's patent grant for WebRTC is libwebrtc/Source/webrtc/PATENTS. WebKit's own build files and changes in libwebrtc/ are Apple Inc.'s, under WebKit's BSD-2-clause licence. The third-party code below has its own licences; libwebrtc/Source/LICENSE_THIRD_PARTY lists more files that carry their own notices."
    stanza 'libwebrtc/WebKit/* libwebrtc/CMakeLists.txt libwebrtc/WebRTCPrefix.h libwebrtc/WebRTCObjCPrefix.h libwebrtc/LibWebRTCWebKitMacros.h.in libwebrtc/cmake/* libwebrtc/Configurations/* libwebrtc/Scripts/*' \
        BSD-2-clause - '2017-2026 Apple Inc.'
    stanza 'libwebrtc/Source/third_party/abseil-cpp/*' Apache-2.0 - '2017-2026 The Abseil Authors' \
        'Text of the licence: libwebrtc/Source/third_party/abseil-cpp/LICENSE.'
    stanza 'libwebrtc/Source/third_party/boringssl/*' Apache-2.0 - \
        '1995-1998 Eric Young|1998-2011 The OpenSSL Project|2014-2026 The BoringSSL Authors|2014-2026 Google Inc.' \
        'BoringSSL. Text of the licence: libwebrtc/Source/third_party/boringssl/src/LICENSE (which also names the few files under other permissive licences, in its third_party/). Built into libWPEWebKit-2.0.so.1 for libwebrtc (DTLS, SRTP keys) with hidden visibility: no OpenSSL symbol is exported, and nothing else in the system links it.'
    stanza 'libwebrtc/Source/third_party/crc32c/*' BSD-3-clause-crc32c "$S/crc32c/src/LICENSE" '2017 The CRC32C Authors'
    stanza 'libwebrtc/Source/third_party/json/*' BSD-2-clause - '2023 Apple Inc.'
    stanza 'libwebrtc/Source/third_party/libaom/*' BSD-2-clause-aom "$S/libaom/source/libaom/LICENSE" \
        '2016-2026 Alliance for Open Media' \
        'The Alliance for Open Media Patent License 1.0 is libwebrtc/Source/third_party/libaom/source/libaom/PATENTS.'
    stanza 'libwebrtc/Source/third_party/libsrtp/*' BSD-3-clause-libsrtp "$S/libsrtp/LICENSE" '2001-2017 Cisco Systems, Inc.'
    stanza 'libwebrtc/Source/third_party/libvpx/*' BSD-3-clause-libvpx "$S/libvpx/source/libvpx/LICENSE" \
        '2010-2026 The WebM Project authors' \
        'Google'"'"'s patent grant for libvpx is libwebrtc/Source/third_party/libvpx/source/libvpx/PATENTS.'
    stanza 'libwebrtc/Source/third_party/libwebm/*' BSD-3-clause-libwebm "$S/libwebm/LICENSE.TXT" '2010-2026 Google Inc.' \
        'Patent grant: libwebrtc/Source/third_party/libwebm/PATENTS.TXT.'
    stanza 'libwebrtc/Source/third_party/libyuv/*' BSD-3-clause-libyuv "$S/libyuv/LICENSE" '2011-2026 The LibYuv Project Authors' \
        'Patent grant: libwebrtc/Source/third_party/libyuv/PATENTS.'
    stanza 'libwebrtc/Source/third_party/opus/*' BSD-3-clause-opus "$S/opus/src/COPYING" \
        '2001-2023 Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon' \
        'Not built: LordOS links Debian'"'"'s libopus.'
    stanza 'libwebrtc/Source/third_party/pffft/*' BSD-like-pffft "$S/pffft/LICENSE" \
        '2013 Julien Pommier|2004 the University Corporation for Atmospheric Research ("UCAR")'
    stanza 'libwebrtc/Source/third_party/rnnoise/*' BSD-3-clause-rnnoise "$S/rnnoise/COPYING" \
        '2017 Mozilla|2007-2017 Jean-Marc Valin|2005-2017 Xiph.Org Foundation|2003-2004 Mark Borgerding'
    stanza 'libwebrtc/Source/third_party/yasm/*' BSD-yasm "$S/yasm/BSD.txt" '2001-2014 Peter Johnson and other Yasm developers' \
        'Not built (the build uses nasm). Yasm is 2- or 3-clause BSD, except bitvect, which is under the Artistic licence, the GPL or the LGPL (libwebrtc/Source/third_party/yasm/COPYING and Artistic.txt).'
} >> "$C"
echo "debian/copyright: $(grep -c '^Files: libwebrtc/' "$C") stanzas for the libwebrtc component"
