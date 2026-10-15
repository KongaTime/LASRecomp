#!/usr/bin/env bash
# LAS Recomp on the PS5 - set up everything the title builds against, beside this repository.
#
#   ps5/tools/bootstrap.sh            check the host, clone what is missing, build what is missing
#   ps5/tools/bootstrap.sh --check    only report what is there and what is not
#
# The title links RADV (Mesa's Vulkan driver for the console, built by PS5_Vulkan from PS5_Mesa) and the platform
# layer of the payload SDK fork, as PS5_VulkanTemplate's and PS5_ReXGlueTemplate's titles do. They are checked out
# beside this repository and built once:
#
#   ../PS5_Vulkan       mihawk-99/PS5_Vulkan     tools/setup-native-dependencies.sh, make (the native tool and
#                                                libc.prx), tools/build-radv.sh release (the RADV archive: a long build)
#   ../PS5_Mesa         mihawk-99/PS5_Mesa       (PS5_Vulkan builds RADV from it, at its pin)
#   ../PS5_PayloadSDK   KongaTime/PS5_PayloadSDK ps5/tools/setup-sdk.sh exports this title's pin of it
#
# then the libraries the title links (ps5/tools/build-deps.sh: shaderc, FFmpeg, SDL's headers, Vulkan-Headers, volk,
# VMA). It never touches the console.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(dirname "$root")
check=false
[[ ${1:-} == --check ]] && check=true
vulkan=${PS5_VULKAN_DIR:-$parent/PS5_Vulkan}
mesa=${PS5_MESA_FORK:-$parent/PS5_Mesa}
sdk_fork=${PS5_PAYLOAD_SDK_FORK:-$parent/PS5_PayloadSDK}
problems=0
ok() { printf '  ok      %s\n' "$*"; }
todo() { printf '  missing %s\n' "$*"; problems=$((problems + 1)); }
step() { printf '==> %s\n' "$*"; }

step "the host (Ubuntu 24.04: the packages in ps5/README.md, \"Building the title\")"
for command in git cmake ninja clang ld.lld python3 meson glslangValidator zstd flex bison make wget unzip curl; do
    command -v "$command" > /dev/null && ok "$command" || todo "$command"
done
command -v nasm > /dev/null && ok "nasm (FFmpeg's assembly)" || echo "  (no nasm: FFmpeg is built with its C code only)"
python3 -c "import mako" 2> /dev/null && ok "python: mako" || todo "python: mako (python3 -m pip install mako)"
clang_major=$(clang --version 2> /dev/null | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
[[ -n $clang_major && $clang_major -ge 18 ]] && ok "clang $clang_major (18 or later)" || todo "clang 18 or later"
builtins="$(clang --print-resource-dir 2> /dev/null)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f $builtins ]] && ok "compiler-rt builtins" || todo "compiler-rt builtins: $builtins (libclang-rt-${clang_major:-18}-dev)"
llvm-config-"${clang_major:-18}" --version > /dev/null 2>&1 || command -v llvm-config > /dev/null &&
    ok "llvm-config (RADV's OpenCL kernels: LLVM, Clang, libclc, the SPIR-V translator)" ||
    todo "LLVM development files for RADV's OpenCL kernels (ps5/README.md)"

step "the projects beside this one"
for spec in "PS5_Vulkan $vulkan mihawk-99" "PS5_Mesa $mesa mihawk-99" "PS5_PayloadSDK $sdk_fork KongaTime"; do
    read -r name path owner <<< "$spec"
    if [[ -d $path/.git ]]; then
        ok "$name ($path, $(git -C "$path" rev-parse --short HEAD))"
    elif $check; then
        todo "$name: git clone https://github.com/$owner/$name.git $path"
    else
        echo "  cloning $owner/$name into $path"
        git clone -q "https://github.com/$owner/$name.git" "$path"
        ok "$name"
    fi
done
# The SDK fork holds two pins: this title's (ps5/tools/setup-sdk.sh: KongaTime's fork) and PS5_Vulkan's own, which
# builds RADV (tools/setup-native-dependencies.sh: mihawk-99's). A checkout of one fork gets the other's commits.
if [[ -d $sdk_fork/.git ]]; then
    for pin in "$(sed -n 's/^sdk_revision=//p' "$root/ps5/tools/setup-sdk.sh") KongaTime" \
            "$(sed -n 's/^sdk_revision=//p' "$vulkan/tools/setup-native-dependencies.sh" 2> /dev/null) mihawk-99"; do
        read -r rev owner <<< "$pin"
        [[ -z $rev ]] && continue
        if git -C "$sdk_fork" cat-file -e "$rev^{commit}" 2> /dev/null; then
            ok "the SDK pin ${rev:0:12}"
        elif $check; then
            todo "the SDK pin ${rev:0:12}: git -C $sdk_fork fetch https://github.com/$owner/PS5_PayloadSDK.git"
        else
            git -C "$sdk_fork" fetch -q "https://github.com/$owner/PS5_PayloadSDK.git" && ok "the SDK pin ${rev:0:12} ($owner)"
        fi
    done
fi
$check && { ((problems)) && exit 1; exit 0; }

step "PS5_Vulkan's build"
(cd "$vulkan" && PS5_PAYLOAD_SDK_FORK="$sdk_fork" bash tools/setup-native-dependencies.sh > /dev/null) || {
    echo "  PS5_Vulkan's payload SDK didn't install: (cd $vulkan && tools/setup-native-dependencies.sh)" >&2
    exit 1
}
ok "its payload SDK"
if [[ ! -x $vulkan/build/host/ps5-native-tool || ! -f $vulkan/runtime/libc.prx ]]; then
    # make builds the native tool and libc.prx first, then PS5_Vulkan's own sample, which a title doesn't need
    (cd "$vulkan" && make > "$vulkan/build-bootstrap-make.log" 2>&1) || true
fi
[[ -x $vulkan/build/host/ps5-native-tool && -f $vulkan/runtime/libc.prx ]] && ok "the native tool and libc.prx" ||
    { echo "  make did not build them: $vulkan/build-bootstrap-make.log" >&2; exit 1; }
if [[ ! -f $vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a ]]; then
    echo "  building RADV (a long build)"
    (cd "$vulkan" && PS5_MESA_FORK="$mesa" bash tools/build-radv.sh release)
fi
ok "the RADV release archive ($(sed -n 's/^revision: //p' "$vulkan/.deps/native/radv-release/PROVENANCE.txt" | cut -c1-11))"

step "this title's SDK pin and libraries"
sdk=$(PS5_PAYLOAD_SDK_FORK="$sdk_fork" bash "$root/ps5/tools/setup-sdk.sh")
ok "the payload SDK at $(cat "$sdk/.ps5-sdk-revision" | cut -c1-12)"
bash "$root/ps5/tools/build-deps.sh" "$sdk" > /dev/null
ok "the libraries (build/ps5-deps)"
echo
echo "Ready: ps5/tools/build.sh builds the title into build/ps5/dist/PPSA99450/."
