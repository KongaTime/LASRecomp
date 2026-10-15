#!/usr/bin/env bash
# LAS Recomp on the PS5 - link the title and lay out its folder.
#
#   link-title.sh BUILD_DIR PS5_VULKAN_DIR SDK INPUT...
#
# Run by the build (ps5/CMakeLists.txt) once the objects are compiled; INPUT is the objects, then the static
# libraries. The link is PS5_Vulkan's, as PS5_ReXGlueTemplate's tools/link-title.sh does it (and this follows that
# script): its RADV release archive, its link recipe (tools/radv-link.sh: the platform layer of the SDK, the heap and
# thread wraps, the libc names bound to ps5_*), its CRT, its native tool (ELF -> the console's fake SELF) and its
# libc.prx. The title folder, build/ps5/dist/<TITLE_ID>/, gets the signed eboot.bin, sce_sys/, sce_module/libc.prx,
# the start screen's font and the notices; no game files, no game code.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
[[ -n ${LINK_TRACE:-} ]] && set -x

work=$1 vulkan=$2 sdk=$3
shift 3
inputs=("$@")
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}
tool="$vulkan/build/host/ps5-native-tool"
native="$vulkan/tooling/native"
param="$root/ps5/sce_sys/param.json"

for file in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" "$vulkan/tools/radv-link.sh" \
        "$vulkan/runtime/libc.prx"; do
    [[ -e $file ]] || { echo "missing: $file (ps5/tools/bootstrap.sh)" >&2; exit 2; }
done
mkdir -p "$work/link/obj" "$work/link/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

# The CRT: main, then catchReturnFromMain (ps5/src/console.c: the shell exit). The C++ runtime is libc++abi's.
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/link/obj/app_crt.o"
# RADV calls AGC, which the SDK has no stubs for: these name its imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/link/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/link/stubs/${library}.so" "$work/link/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2
# localeconv in the C locale, '.' the decimal point (the console's gives none, and every fraction the settings
# file holds would read as 0): bound here when the recipe doesn't yet, kept local as the recipe keeps its names.
# The symbol listing is read whole before it is searched (llvm-nm's SIGPIPE under pipefail: PS5_ReXGlueTemplate).
platform_has_localeconv=false
grep -q " T ps5_localeconv$" <<<"$("$sdk/bin/llvm-nm" --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null)" &&
    platform_has_localeconv=true
if $platform_has_localeconv && [[ " ${radv_link_flags[*]} " != *" --defsym=localeconv=ps5_localeconv "* ]]; then
    printf '{\n    local:\n        localeconv;\n};\n' > "$work/link/localeconv-local.map"
    radv_link_flags+=(--defsym=localeconv=ps5_localeconv --version-script "$work/link/localeconv-local.map")
fi
# libc names the title calls that the console lacks, refuses or faults in, beyond those the recipe binds: getcwd
# (libc's faults: the __getcwd it calls is libkernel_sys's) and realpath (refused, even for /app0), which the runtime
# calls and libc++'s std::filesystem too; FFmpeg's isatty; libc++'s fork and pathconf. The platform layer's ps5_
# versions, bound and kept local as the recipe binds its own (ps5platform/libc.h).
bound=()
platform_symbols=$("$sdk/bin/llvm-nm" --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null || true)
for name in getcwd realpath fork isatty pathconf; do
    grep -q " T ps5_$name$" <<<"$platform_symbols" || continue
    [[ " ${radv_link_flags[*]} " == *" --defsym=$name=ps5_$name "* ]] && continue
    radv_link_flags+=("--defsym=$name=ps5_$name")
    bound+=("$name")
done
if ((${#bound[@]})); then
    { printf '{\n    local:\n'; printf '        %s;\n' "${bound[@]}"; printf '};\n'; } > "$work/link/title-bound-local.map"
    radv_link_flags+=(--version-script "$work/link/title-bound-local.map")
fi
# The title's ending (ps5/src/title.c): exit and its kin ask the shell to close the title
title_flags=(--wrap=exit --wrap=_exit --wrap=_Exit --wrap=atexit)
# The working directory the title keeps where the console refuses chdir (ps5/src/cwd.c): every path call wrapped
for name in chdir getcwd open fopen stat lstat mkdir rmdir unlink remove rename chmod truncate utime utimes access \
        opendir realpath; do
    title_flags+=("--wrap=$name")
done
# (the wraps stay the title's own: a name the SDK's libc stubs also use would otherwise be exported, which the
# converter refuses)
printf '{\n    local:\n        __wrap_*;\n};\n' > "$work/link/title-wraps-local.map"
title_flags+=(--version-script "$work/link/title-wraps-local.map")
# RADV's dispatch tables name every entry point weakly, most of them defined by no one: those are null, statically.
# Left as dynamic imports, the converter would look for each in the SDK's stubs and refuse the title. As PS5_Vulkan
# links its driver tests (tools/build.sh): --no-dynamic-linker keeps them out of the dynamic symbols (lld 18), and
# -z nodynamic-undefined-weak says so to the lld versions that have it.
title_flags+=(--no-dynamic-linker)
ld_version=$("$(dirname "$("$sdk/bin/prospero-llvm-config" --bindir)/x")/ld.lld" --version 2>/dev/null | sed -n 's/.*LLD \([0-9]*\).*/\1/p')
[[ ${ld_version:-0} -ge 19 ]] && title_flags+=(-z nodynamic-undefined-weak)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" -T "$root/ps5/tools/sections.ld" --eh-frame-hdr "${radv_link_flags[@]}" "${title_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL --no-dependent-libraries \
    -e _start -o "$work/link/llvm-pie.elf" \
    "$work/link/obj/app_crt.o" --start-group "${inputs[@]}" --end-group \
    "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
if $platform_has_localeconv &&
        grep -q " U localeconv$" <<<"$("$sdk/bin/llvm-nm" "$work/link/llvm-pie.elf" 2>/dev/null)"; then
    echo "link-title.sh: localeconv is not bound to the platform layer's ps5_localeconv" >&2
    exit 1
fi
# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's: an import only their stubs define links,
# and is null at run time (its first call jumps to address 0). Refused here rather than on the console.
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/link/llvm-pie.elf" |
        awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "link-title.sh: imports that no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    echo "link-title.sh: bind them to the platform layer (the bound list above)" >&2
    exit 1
fi
"$tool" link --in "$work/link/llvm-pie.elf" --out "$work/eboot.elf.new" \
    --stub-dir "$sdk/target/lib" --stub "$work/link/stubs/libSceAgc.so" \
    --stub "$work/link/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

# The title folder
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app="$work/dist/$title_id"
rm -rf "$app.new"
mkdir -p "$app.new/sce_sys" "$app.new/sce_module" "$app.new/licenses"
"$tool" self --sign --in "$work/eboot.elf.new" --out "$app.new/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app.new/eboot.bin" > /dev/null
cp "$param" "$root/ps5/sce_sys/icon0.png" "$app.new/sce_sys/"
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app.new/sce_module/libc.prx"
# what the runtime finds next to its executable (rt_resource): the start screen's font (OFL)
cp "$root/third_party/fonts/Baloo2/Baloo2-SemiBold.ttf" "$app.new/"
# and the game's system font where no Switch font was imported (runtime/hle/pl.c)
cp "$root/build/ps5-deps/prefix/fonts/DejaVuSans.ttf" "$app.new/"
cp "$root/ps5/HOW-TO-INSTALL.txt" "$app.new/"
# The notices: this project's licence and README (its "License" section names every bundled part), and each part
# the title links, with its revision
python3 "$root/ps5/tools/stage-notices.py" "$app.new" "$vulkan" "$archive" "$sdk" "$root/build/ps5-deps"
mv "$work/eboot.elf.new" "$work/eboot.elf"
# (the folder replaced whole, keeping what a player put into it on the PC: extracted/, save/, larecomp.ini)
mkdir -p "$app"
rm -rf "$app/eboot.bin" "$app/sce_sys" "$app/sce_module" "$app/licenses"
cp -R "$app.new/." "$app/"
rm -rf "$app.new"
archive_revision=$(sed -n 's/^revision: //p' "$(dirname "$(dirname "$archive")")/PROVENANCE.txt" 2>/dev/null || true)
printf '==> %s: %s (eboot.bin %s bytes; RADV %s)\n' "$title_id" "$app" \
    "$(stat -c %s "$app/eboot.bin")" "${archive_revision:0:11}"
