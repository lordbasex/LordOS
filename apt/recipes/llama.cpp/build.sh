# apt/recipes/llama.cpp/build.sh — llama.cpp built from the pinned upstream source release.
# Portable: GGML_NATIVE=OFF, every CPU variant of the architecture built as a backend module
# (GGML_BACKEND_DL + GGML_CPU_ALL_VARIANTS) and the best one loaded at run time, plus the Vulkan
# backend as another module (GGML_VULKAN), used only when a Vulkan device exists. HTTPS (OpenSSL) for
# "-hf" model downloads. Programs in
# /usr/bin, libraries and backend modules in /usr/lib/llama.cpp (RUNPATH), nothing for development.
# llama-server embeds the web UI that upstream's CI built from the same commit (a pinned input), since
# the build has no network (upstream's CMake would otherwise download it unverified).
. /recipes/lib.sh

app=/in/app.json
commit=$(jq -r .source.commit "$app"); bn=$(jq -r .source.build_number "$app")
w=$(mktemp -d); r=$w/root; lib=/usr/lib/llama.cpp
mkdir -p "$w/src" "$r/DEBIAN"
tar -xzf /in/source.tar.gz -C "$w/src" --strip-components=1 --no-same-owner
mkdir -p "$w/src/tools/ui/dist"
tar -xzf /in/llama-ui.tar.gz -C "$w/src/tools/ui/dist" --strip-components=1 --no-same-owner
[ -f "$w/src/tools/ui/dist/index.html" ] || die "the UI archive has no index.html"

cmake -S "$w/src" -B "$w/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/llama.cpp \
    -DCMAKE_INSTALL_RPATH="$lib" -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$w=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$w=." \
    -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON \
    -DGGML_BACKEND_DIR="$lib" -DGGML_VULKAN=ON \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=ON \
    -DLLAMA_BUILD_IS_DEV=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF -DLLAMA_OPENSSL=ON \
    -DLLAMA_BUILD_NUMBER="$bn" -DLLAMA_BUILD_COMMIT="$(printf %s "$commit" | cut -c1-7)" >&2
cmake --build "$w/build" -j "$JOBS" >&2
DESTDIR=$r cmake --install "$w/build" >&2

# runtime only: no headers, CMake or pkg-config files, no Python conversion scripts (they need
# Python packages Debian does not have, such as torch), no unversioned .so links
rm -rf "$r/usr/include" "$r$lib/cmake" "$r$lib/pkgconfig"
find "$r/usr/bin" -name '*.py' -delete
find "$r$lib" -name '*.so' -type l -delete
ls "$r/usr/bin/llama-cli" "$r/usr/bin/llama-server" >/dev/null || die "llama-cli or llama-server missing"
ls "$r$lib"/libggml-cpu-*.so >/dev/null 2>&1 || die "no CPU backend modules in $lib"
[ -f "$r$lib/libggml-vulkan.so" ] || die "no Vulkan backend module in $lib"

mkdir -p "$r/usr/share/doc/$PKG"
copyright_file "$r/usr/share/doc/$PKG/copyright" llama.cpp https://github.com/ggml-org/llama.cpp MIT "$w/src/LICENSE"
[ -d "$w/src/licenses" ] && cp "$w/src/licenses/"* "$r/usr/share/doc/$PKG/" 2>/dev/null || true

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
Recommends: ca-certificates, mesa-vulkan-drivers
Section: science
Priority: optional
Homepage: https://github.com/ggml-org/llama.cpp
Description: LLM inference in C/C++: run GGUF models locally
 llama.cpp runs large language models in the GGUF format on the CPU or a
 Vulkan GPU:
 llama-cli for a chat in the terminal, llama-server for an OpenAI-compatible
 HTTP API with a web UI, and tools to quantize, benchmark and convert models.
 "llama-cli -hf <user>/<model>-GGUF" downloads a model from Hugging Face.
 .
 Built by LordOS from the upstream release $(jq -r .source.tag "$app") (commit $(printf %s "$commit" | cut -c1-12)), with
 every CPU variant of the architecture (the best one is chosen at run time)
 and the Vulkan backend, loaded only when a Vulkan device is present (a GPU
 with a Vulkan driver, such as Mesa's in mesa-vulkan-drivers).
 Programs: $bins
EOF
build_deb "$r"
