#!/usr/bin/env bash
# LAS Recomp on the PS5 - the libraries the title links, built for the console into build/ps5-deps/.
#
#   ps5/tools/build-deps.sh SDK
#
# SDK: the payload SDK fork (ps5/tools/setup-sdk.sh prints it). The same versions as the Linux build
# (tools/linux/build_deps.sh), from their git tags:
#   - SDL3's headers only: ps5/sdl/sdl_ps5.c implements the part of SDL the runtime uses over the console's own
#     services, against SDL's own declarations;
#   - Vulkan-Headers, volk (the title's Vulkan commands, from the RADV linked in) and VMA;
#   - shaderc with its glslang and SPIRV-Tools (its DEPS file pins them), as static libraries;
#   - FFmpeg, the LGPL build with the Linux build's decoders (the game's films), as static libraries.
# Each is built once; delete its folder in build/ps5-deps/ to build it again.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(cd -- "${1:?usage: build-deps.sh SDK}" && pwd)
W=$root/build/ps5-deps
S=$W/src
P=$W/prefix
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
mkdir -p "$S" "$P/include" "$P/lib"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

# a tag's tree, its commit checked (the tag is the pin; the commit says it is the one these builds were made with)
fetch() {  # url tag commit dir
    [[ -d $S/$4 ]] && return
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$S/$4.tmp"
    local got
    got=$(git -C "$S/$4.tmp" rev-parse HEAD)
    [[ $got == "$3" ]] || { echo "$4: $2 is $got, expected $3" >&2; rm -rf "$S/$4.tmp"; exit 1; }
    mv "$S/$4.tmp" "$S/$4"
}
fetch https://github.com/libsdl-org/SDL.git release-3.4.16 fa2c02bb6e21974a89ea9824bc53c9932abe5f9c sdl
fetch https://github.com/KhronosGroup/Vulkan-Headers.git vulkan-sdk-1.4.350.1 8864cdc896bbc2a9b6eb36b3218fc9ef57908d77 vulkan-headers
fetch https://github.com/zeux/volk.git vulkan-sdk-1.4.350.1 3ca312a4f38baa63d8006b6905abbeeb89c8087d volk
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0 1d8f600fd424278486eade7ed3e877c99f0846b1 vma
fetch https://github.com/google/shaderc.git v2026.3 2c8cae778eec0283b44acbe7ed1a386865d78799 shaderc
fetch https://github.com/FFmpeg/FFmpeg.git n9.0.2 946fcce07b6dcd0331c8cc609192aeff5e1924f8 ffmpeg

# The licence of the LLVM runtime the title links from the SDK (libc++, libc++abi, libunwind, compiler-rt), for
# its notices (ps5/tools/stage-notices.py)
if [[ ! -f $P/llvm/LICENSE.TXT ]]; then
    mkdir -p "$P/llvm"
    curl -sSfL -o "$P/llvm/LICENSE.TXT.tmp" https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.3/llvm/LICENSE.TXT
    echo "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee  $P/llvm/LICENSE.TXT.tmp" | sha256sum --check --strict --quiet
    mv "$P/llvm/LICENSE.TXT.tmp" "$P/llvm/LICENSE.TXT"
fi
if [[ ! -f $P/include/SDL3/SDL.h ]]; then
    cp -R "$S/sdl/include/SDL3" "$P/include/"
fi
if [[ ! -f $P/include/vulkan/vulkan.h ]]; then
    cp -R "$S/vulkan-headers/include/vulkan" "$S/vulkan-headers/include/vk_video" "$P/include/"
fi
if [[ ! -f $P/volk/volk.c ]]; then
    mkdir -p "$P/volk" && cp "$S/volk/volk.c" "$S/volk/volk.h" "$S/volk/LICENSE.md" "$P/volk/"
fi
if [[ ! -f $P/include/vma/vk_mem_alloc.h ]]; then
    mkdir -p "$P/include/vma" && cp "$S/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi

# shaderc, glslang and SPIRV-Tools for the console (C++17, static), with the SDK's CMake toolchain
if [[ ! -f $P/lib/libshaderc_combined.a ]]; then
    (cd "$S/shaderc" && python3 utils/git-sync-deps)
    cmake -S "$S/shaderc" -B "$W/shaderc" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
        -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF \
        -DSPIRV_WERROR=OFF -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" > "$W/shaderc.configure.log"
    cmake --build "$W/shaderc" --target shaderc_combined -j"$JOBS" > "$W/shaderc.build.log"
    mkdir -p "$P/include/shaderc"
    cp "$S/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp "$W/shaderc/libshaderc/libshaderc_combined.a" "$P/lib/"
fi

# FFmpeg: LGPL, static, the Linux build's demuxers and decoders. Its x86 assembly needs nasm (left out without: the
# C decoders are slower, not different)
if [[ ! -f $P/lib/libavcodec.a ]]; then
    rm -rf "$W/ffmpeg" && cp -R "$S/ffmpeg" "$W/ffmpeg"
    asm=(--disable-x86asm)
    command -v nasm > /dev/null && asm=()
    (cd "$W/ffmpeg" && ./configure --prefix="$P" --enable-cross-compile --target-os=freebsd --arch=x86_64 \
        --cc="$sdk/bin/prospero-clang" --cxx="$sdk/bin/prospero-clang++" --ld="$sdk/bin/prospero-clang" \
        --ar="$sdk/bin/llvm-ar" --nm="$sdk/bin/llvm-nm" --ranlib="$sdk/bin/llvm-ranlib" --strip="$sdk/bin/llvm-strip" \
        --extra-cflags="-fno-omit-frame-pointer" "${asm[@]}" \
        --enable-static --disable-shared --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        > "$W/ffmpeg.configure.log" && make -j"$JOBS" > "$W/ffmpeg.build.log" && make install > /dev/null)
fi
grep -q 'GPL version 3\|--enable-gpl' "$P/lib/pkgconfig/libavcodec.pc" && { echo "deps: FFmpeg is not LGPL"; exit 1; }
echo "deps: $P"
