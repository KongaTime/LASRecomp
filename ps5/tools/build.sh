#!/usr/bin/env bash
# LAS Recomp on the PS5 - build the title into build/ps5/dist/<TITLE_ID>/.
#
#   ps5/tools/build.sh
#
# Configures ps5/ with the payload SDK's CMake toolchain (once), builds the libraries it links (ps5/tools/build-deps.sh,
# once), compiles what changed, links with RADV and lays out the title folder (ps5/tools/link-title.sh).
#
#   PS5_VULKAN_DIR         the PS5_Vulkan checkout (default ../PS5_Vulkan): the RADV release archive, the link recipe,
#                          the native tool and libc.prx (ps5/tools/bootstrap.sh builds them)
#   PS5_PAYLOAD_SDK_FORK   the payload SDK fork (default ../PS5_PayloadSDK): ps5/tools/setup-sdk.sh's pin
#   PS5_CLANG              the host clang the compiler wrappers drive (default: clang; 18 or later)
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$root/ps5/tools/setup-sdk.sh")
build="$root/build/ps5"
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

bash "$root/ps5/tools/build-deps.sh" "$sdk"
if [[ ! -f $build/build.ninja ]]; then
    mkdir -p "$build"
    cmake -S "$root/ps5" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DPS5_VULKAN_DIR="$vulkan" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF > "$build.configure.log" 2>&1 \
        || { cat "$build.configure.log" >&2; exit 1; }
fi
"$ninja" -C "$build" title
