# 0.8.7: the login screen composes on the GPU and decodes its moving wallpaper in hardware
# (docs/WALLPAPERS.md): lordos-greeter joins render and video. On the Raspberry Pi, FFmpeg comes from Raspberry
# Pi's archive, the only one that drives the Pi 5's HEVC decoder; the next update brings it.
# LORDOS_MIGRATION_ROOT is for tests only.
R=${LORDOS_MIGRATION_ROOT:-}
if [ -z "$R" ] && getent passwd lordos-greeter >/dev/null 2>&1; then
    for g in render video; do
        getent group "$g" >/dev/null 2>&1 && usermod -aG "$g" lordos-greeter
    done
fi
P="$R/etc/apt/preferences.d/raspberrypi"
if [ -f "$P" ] && ! grep -q 'libpostproc58' "$P"; then
    t=$(mktemp "$P.XXXXXX")
    if grep -q 'libavcodec61' "$P"; then    # a development build's list, without the last one
        sed 's/ libavdevice61 / libavdevice61 libpostproc58 /' "$P" > "$t"
    else
        sed 's/^Package: raspi-firmware /Package: raspi-firmware ffmpeg libavcodec61 libavformat61 libavutil59 libswscale8 libswresample5 libavfilter10 libavdevice61 libpostproc58 /' "$P" > "$t"
    fi
    chmod 644 "$t"
    mv "$t" "$P"
fi
exit 0
