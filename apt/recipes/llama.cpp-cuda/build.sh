# apt/recipes/llama.cpp-cuda/build.sh — llama.cpp's CUDA backend module only (libggml-cuda.so), for NVIDIA cards, as
# its own optional package next to LordOS's llama.cpp (docs/AI.md, "The Brain on the GPU").
#
# The same pinned upstream source as apt/recipes/llama.cpp (the catalog entry must name the same commit), configured
# the same way (GGML_BACKEND_DL: every backend a module ggml loads at run time from /usr/lib/llama.cpp) with GGML_CUDA
# on, but only the ggml-cuda target is built and only its module is packaged: llama.cpp's own package keeps libggml,
# the CPU modules and Vulkan; this one adds the CUDA module, which ggml loads only when it is there and its runtime
# loads (libcudart and libcublas from NVIDIA's CUDA repository, libcuda from NVIDIA's driver).
#
# Runs in the "nvidia-cuda" build image (apt/build.sh build_image): debian:trixie plus NVIDIA's CUDA repository for
# Debian 13 (key apt/keys/nvidia-cuda.asc, fingerprint 0218 2E60 104F CDC2 6EAE 1B85 97A5 D4CB 8793 F200) and the
# compiler and development files of CUDA $CUDA_SERIES. No network while building.
#
# Cards: Turing and newer (CUDA 13 dropped Maxwell, Pascal and Volta, which NVIDIA's repository doesn't drive either;
# docs/NVIDIA-PLAN.md A.2): real code for Ampere (8.6), Ada (8.9) and Blackwell (12.0), PTX for Turing (7.5), A100
# (8.0) and Hopper (9.0), compiled by the driver at first use.
. /recipes/lib.sh

CUDA_SERIES=13-5                    # NVIDIA's package names: cuda-cudart-13-5, libcublas-13-5
CUDA_DIR=/usr/local/cuda-13.5
app=/in/app.json
commit=$(jq -r .source.commit "$app"); bn=$(jq -r .source.build_number "$app")
w=$(mktemp -d); r=$w/root; lib=/usr/lib/llama.cpp
mkdir -p "$w/src" "$r/DEBIAN" "$r$lib"
tar -xzf /in/source.tar.gz -C "$w/src" --strip-components=1 --no-same-owner
[ -x "$CUDA_DIR/bin/nvcc" ] || die "no nvcc in $CUDA_DIR: build in the nvidia-cuda image"

cmake -S "$w/src" -B "$w/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/llama.cpp \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$w=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$w=." \
    -DCMAKE_CUDA_COMPILER="$CUDA_DIR/bin/nvcc" -DCMAKE_CUDA_ARCHITECTURES="75-virtual;80-virtual;86-real;89-real;90-virtual;120-real" \
    -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON \
    -DGGML_BACKEND_DIR="$lib" -DGGML_CUDA=ON -DGGML_VULKAN=OFF \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_BUILD_IS_DEV=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF -DLLAMA_OPENSSL=OFF \
    -DLLAMA_BUILD_NUMBER="$bn" -DLLAMA_BUILD_COMMIT="$(printf %s "$commit" | cut -c1-7)" >&2
cmake --build "$w/build" -j "$JOBS" --target ggml-cuda >&2
m=$(find "$w/build" -name libggml-cuda.so -type f | head -n1)
[ -n "$m" ] || die "no libggml-cuda.so was built"
install -m 0644 "$m" "$r$lib/libggml-cuda.so"
# libggml-base (llama.cpp's package, the same commit) next to it; CUDA's libraries from NVIDIA's packages, which put
# them in $CUDA_DIR (not on the loader's path); libcuda.so.1 from the driver, on the system's path
patchelf --set-rpath "$lib:$CUDA_DIR/targets/x86_64-linux/lib" "$r$lib/libggml-cuda.so" 2>/dev/null ||
    die "patchelf is needed to set the module's RUNPATH"
readelf -d "$r$lib/libggml-cuda.so" | grep -q 'NEEDED.*libcudart.so' || die "the module doesn't link libcudart"

mkdir -p "$r/usr/share/doc/$PKG"
copyright_file "$r/usr/share/doc/$PKG/copyright" llama.cpp https://github.com/ggml-org/llama.cpp MIT "$w/src/LICENSE"

cat > "$r/DEBIAN/control" <<CONTROL
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $(installed_size "$r")
Depends: llama.cpp (= $VERSION), cuda-cudart-$CUDA_SERIES, libcublas-$CUDA_SERIES, libcuda1 (>= 590), libc6, libstdc++6, libgcc-s1
Section: contrib/science
Priority: optional
Homepage: https://github.com/ggml-org/llama.cpp
Description: llama.cpp's CUDA backend, for NVIDIA cards
 The CUDA module of LordOS's llama.cpp (libggml-cuda.so in /usr/lib/llama.cpp).
 llama.cpp loads it at run time when it is installed and NVIDIA's driver and
 CUDA runtime are there, and then runs models on the NVIDIA card; without it
 llama.cpp uses Vulkan or the processor.
 .
 It needs NVIDIA's own driver and CUDA runtime, from NVIDIA's CUDA repository
 for Debian 13 (Turing and newer cards), which LordOS adds only when the person
 chooses NVIDIA's driver in the Store. Built by LordOS from the upstream release
 $(jq -r .source.tag "$app") (commit $(printf %s "$commit" | cut -c1-12)), the same as the llama.cpp package.
CONTROL
build_deb "$r"
