# apt/recipes/whisper.cpp/build.sh — whisper.cpp built from the pinned upstream source release, like
# llama.cpp: CPU only and portable (GGML_NATIVE=OFF, every CPU variant as a backend module loaded at
# run time), programs in /usr/bin, libraries in /usr/lib/whisper.cpp (RUNPATH). SDL2 is on, for the
# microphone examples (whisper-stream, whisper-command). No network during the build.
. /recipes/lib.sh

app=/in/app.json
commit=$(jq -r .source.commit "$app")
w=$(mktemp -d); r=$w/root; lib=/usr/lib/whisper.cpp
mkdir -p "$w/src" "$r/DEBIAN" "$r/usr/bin"
tar -xzf /in/source.tar.gz -C "$w/src" --strip-components=1 --no-same-owner
# whisper-talk-llama would clone llama.cpp from git at configure time (unverified, and there is no
# network here): left out
grep -q 'add_subdirectory(talk-llama)' "$w/src/examples/CMakeLists.txt" || die "examples/CMakeLists.txt changed: check talk-llama"
sed -i '/add_subdirectory(talk-llama)/d' "$w/src/examples/CMakeLists.txt"

cmake -S "$w/src" -B "$w/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/whisper.cpp \
    -DCMAKE_INSTALL_RPATH="$lib" -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$w=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$w=." \
    -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON \
    -DGGML_BACKEND_DIR="$lib" \
    -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DWHISPER_BUILD_SERVER=ON \
    -DWHISPER_BUILD_IS_DEV=OFF -DWHISPER_SDL2=ON -DWHISPER_CURL=OFF -DWHISPER_COMMON_FFMPEG=OFF \
    -DWHISPER_BUILD_COMMIT="$(printf %s "$commit" | cut -c1-7)" >&2
cmake --build "$w/build" -j "$JOBS" >&2
DESTDIR=$r cmake --install "$w/build" >&2

# every whisper-* program that was built (only some have install rules upstream), with the RUNPATH
# of the installed ones; the deprecated short names ("main", "bench", …) are left out
for b in "$w/build/bin"/whisper-*; do
    [ -f "$b" ] && [ -x "$b" ] || continue
    n=$(basename "$b")
    [ -f "$r/usr/bin/$n" ] && continue
    install -m 0755 "$b" "$r/usr/bin/$n"
done
rm -rf "$r/usr/include" "$r$lib/cmake" "$r$lib/pkgconfig"
find "$r$lib" -name '*.so' -type l -delete
find "$r/usr/bin" -type f ! -name 'whisper-*' -delete
ls "$r/usr/bin/whisper-cli" "$r/usr/bin/whisper-server" >/dev/null || die "whisper-cli or whisper-server missing"
ls "$r$lib"/libggml-cpu-*.so >/dev/null 2>&1 || die "no CPU backend modules in $lib"
# the build tree's RUNPATH of the copied programs → the installed one
for b in "$r/usr/bin"/whisper-*; do
    rp=$(readelf -d "$b" 2>/dev/null | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')
    [ "$rp" = "$lib" ] && continue
    die "$(basename "$b") has RUNPATH '$rp', expected $lib"
done
# the model download script (curl/wget from Hugging Face), with a writable default folder
install -D -m 0755 "$w/src/models/download-ggml-model.sh" "$r/usr/share/whisper.cpp/download-ggml-model.sh"

mkdir -p "$r/usr/share/doc/$PKG"
copyright_file "$r/usr/share/doc/$PKG/copyright" whisper.cpp https://github.com/ggml-org/whisper.cpp MIT "$w/src/LICENSE"

# shellcheck disable=SC2086
deps=$(shlibs_depends "$r" "$lib" -- $(elf_files "$r/usr/bin" "$r$lib"))
bins=$(cd "$r/usr/bin" && ls | tr '\n' ' ')
cat > "$r/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $(installed_size "$r")
Depends: $deps
Recommends: curl | wget
Section: sound
Priority: optional
Homepage: https://github.com/ggml-org/whisper.cpp
Description: speech recognition with OpenAI's Whisper models, in C/C++
 whisper.cpp transcribes and translates speech on the CPU with Whisper models
 in the ggml format: whisper-cli for audio files, whisper-server for an HTTP
 API, whisper-stream for the microphone in real time.
 .
 Download a model with
 "/usr/share/whisper.cpp/download-ggml-model.sh base.en ~/models" and run
 "whisper-cli -m ~/models/ggml-base.en.bin -f audio.wav".
 .
 Built by LordOS from the upstream release $(jq -r .source.tag "$app") (commit $(printf %s "$commit" | cut -c1-12)), CPU
 only, with every CPU variant of the architecture. Programs: $bins
EOF
build_deb "$r"
