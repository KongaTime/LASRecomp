#!/usr/bin/env bash
# LAS Recomp on the PS5 - the payload SDK the title builds with, at a pinned revision; prints its folder.
#
#   ps5/tools/setup-sdk.sh       installs it into build/ps5-sdk (once a revision)
#
# The SDK is the PS5 payload SDK fork with its platform layer (libps5platform.a, include/ps5platform), checked out
# beside this repository (../PS5_PayloadSDK, or PS5_PAYLOAD_SDK_FORK; ps5/tools/bootstrap.sh clones it). The pinned
# revision is exported with git archive, so a build never depends on the fork's working tree, as
# PS5_VulkanTemplate and PS5_ReXGlueTemplate pin theirs. RADV's archive, its link recipe and the packaging tools
# stay PS5_Vulkan's.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk="$root/build/ps5-sdk"
sdk_fork="${PS5_PAYLOAD_SDK_FORK:-$root/../PS5_PayloadSDK}"
# 1de8b37 (KongaTime/PS5_PayloadSDK): the revision PS5_RPCS3Title and PS5_ReXGlueTemplate pin, the platform
# layer's pathconf and sbrk on PS5_VulkanTemplate's adc8dd7. Never older than fa69d00 (ps5_localeconv).
sdk_revision=1de8b37d583de476ad07cde9a0d562b75fb1be8f
# The upstream SDK release the fork's setup installs under the platform layer (downloaded once, kept here)
cache="${PS5_SDK_CACHE:-$root/build/ps5-sdk-cache}"

if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    git -C "$sdk_fork" cat-file -e "$sdk_revision^{commit}" 2>/dev/null || {
        echo "the payload SDK fork at $sdk_fork does not have $sdk_revision (git -C $sdk_fork fetch)" >&2
        exit 2
    }
    mkdir -p "$cache"
    sdk_tree=$(mktemp -d)
    git -C "$sdk_fork" archive "$sdk_revision" | tar -x -C "$sdk_tree"
    bash "$sdk_tree/platform/tools/setup-sdk.sh" "$sdk" "$sdk_revision" "$cache" >&2
    rm -rf -- "$sdk_tree"
fi
[[ -x $sdk/bin/prospero-lld && -d $sdk/target/include ]] || { echo "the pinned PS5 payload SDK is incomplete" >&2; exit 2; }
echo "$sdk"
