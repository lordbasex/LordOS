#!/bin/sh
# backport.sh <folder of these files> — WebRTC on WebKit's LibWebRTC backend instead of its GStreamer one
# (WEBRTC_BACKEND=libwebrtc, since 2.54.3-1~lordos13+5; docs/WPEWEBKIT.md "LibWebRTC"). Run by
# tools/pkg/wpewebkit-chroot.sh in the unpacked source tree, after backport() and with the libwebrtc component already
# in libwebrtc/. Maintainer's decision of 2026-11-26 ("option B"): WebKit's in-tree libwebrtc, with its bundled
# BoringSSL, libvpx and libaom, linked only inside libWPEWebKit-2.0.so.1. ICE and every socket of a call stay in WebKit's
# network process (NetworkRTCProvider, LibWebRTCSocket), so the web process keeps bubblewrap's --unshare-net.
# Every change checks its anchor: one that stops applying stops the build and says which.
set -eu
H=${1:?usage: backport.sh <folder of tools/pkg/wpewebkit-libwebrtc>}
die() { echo "wpewebkit-libwebrtc/backport.sh: $*" >&2; exit 1; }
[ -f libwebrtc/CMakeLists.txt ] || die "no libwebrtc component in $(pwd)/libwebrtc"

# 1. cmake: the GStreamer backend off (then WebKit's CMake sets USE_LIBWEBRTC), no librice (only the GStreamer
#    backend uses it)
grep -q '^	   -DUSE_GSTREAMER_WEBRTC=ON \\$' debian/rules || die "debian/rules: no -DUSE_GSTREAMER_WEBRTC=ON line"
grep -q '^	   -DUSE_LIBRICE=ON' debian/rules || die "debian/rules: no -DUSE_LIBRICE=ON line"
sed -i 's/^	   -DUSE_GSTREAMER_WEBRTC=ON \\$/	   -DUSE_GSTREAMER_WEBRTC=OFF \\/; s/^	   -DUSE_LIBRICE=ON/	   -DUSE_LIBRICE=OFF/' debian/rules
# 2. WebKit's CMake looks for it in Source/ThirdParty/libwebrtc, where the release tarball has nothing: the component
#    (libwebrtc/ at the top of the source package) is linked there before cmake runs, and the link goes on clean
grep -q '^override_dh_auto_configure:$' debian/rules || die "debian/rules: no override_dh_auto_configure"
grep -q '^override_dh_auto_clean: debian/control$' debian/rules || die "debian/rules: no override_dh_auto_clean"
sed -i 's|^override_dh_auto_configure:$|override_dh_auto_configure:\n\tln -sfn ../../libwebrtc Source/ThirdParty/libwebrtc|; s|^override_dh_auto_clean: debian/control$|override_dh_auto_clean: debian/control\n\trm -f Source/ThirdParty/libwebrtc|' debian/rules
# 3. build dependencies: libwebrtc's on Linux (libevent, ALSA, Opus, PulseAudio; nasm for libvpx's x86 assembly) in place
#    of the GStreamer backend's (OpenSSL for SFrame, librice)
grep -q '^               libssl-dev,$' debian/control || die "debian/control: no libssl-dev"
grep -q '^               librice-dev (>= 0.4.3),$' debian/control || die "debian/control: no librice-dev"
sed -i '/^               librice-dev (>= 0.4.3),$/d; s/^               libssl-dev,$/               libasound2-dev,\
               libevent-dev,\
               libopus-dev,\
               libpulse-dev,\
               nasm [amd64],/' debian/control
# 4. at run time: the GStreamer elements the LibWebRTC backend still uses (capture, the H.264 and VP8 encoders and
#    decoders through WebKit's GStreamer codec factories); libnice is no longer one of them
grep -q '^Recommends: gstreamer1.0-nice, ' debian/control || die "debian/control: no WebRTC Recommends"
sed -i 's/^Recommends: gstreamer1.0-nice, /Recommends: /' debian/control
# 5. two fixes to WebKit's CMake build of libwebrtc on Linux, and VP8 from libwebrtc's own encoder (the patches say why)
for p in lordos-libwebrtc-vpx-include.patch lordos-libwebrtc-vpx-cpu-flags.patch lordos-libwebrtc-vp8-encoder.patch; do
    cp "$H/$p" debian/patches/ && echo "$p" >> debian/patches/series
done
# 6. debian/copyright: the component's licences
sh "$H/copyright.sh" "$(pwd)"
# 7. the changelog entry made by backport() says what this build does instead
for l in "Build WebRTC with WebKit's GStreamer backend" "ICE with librice (USE_LIBRICE)"; do
    grep -q "^  \* $l" debian/changelog || die "debian/changelog: no \"$l\" line"
done
# an item of the first entry and its continuation lines (dch wraps them) are replaced as a whole
awk '
    /^ -- / { done = 1 }
    !done && /^  \* Build WebRTC with WebKit.s GStreamer backend/ {
        print "  * Build WebRTC with WebKit\047s LibWebRTC backend (its own libwebrtc, from WebKit\047s git at the release"
        print "    tag, as the libwebrtc component of the source package) and MediaStream; Build-Depends on libevent,"
        print "    ALSA, Opus, PulseAudio and nasm; Recommends the GStreamer plugins it uses (bad, good, base)."
        skip = 1; next
    }
    !done && /^  \* ICE with librice \(USE_LIBRICE\)/ {
        print "  * ICE and every socket of a call in WebKit\047s network process (NetworkRTCProvider), so the web process"
        print "    keeps no network."
        print "  * lordos-libwebrtc-vpx-include.patch, lordos-libwebrtc-vpx-cpu-flags.patch: libwebrtc builds on Linux,"
        print "    and libvpx\047s instruction set flags are per file."
        skip = 1; next
    }
    skip && /^    / { next }
    { skip = 0; print }
' debian/changelog > debian/changelog.new && mv debian/changelog.new debian/changelog
grep -q 'LibWebRTC backend' debian/changelog || die "could not rewrite the changelog entry"
echo "LibWebRTC backend: rules, control, patches, copyright and changelog done"
