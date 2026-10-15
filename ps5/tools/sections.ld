/*
 * LAS Recomp on the PS5 - where the runtime's registries go (ps5/tools/link-title.sh adds this to PS5_Vulkan's
 * layout). RT_ON_RESET, HLE_FUNC and SERVICE (runtime/hle.h, runtime/ipc.h) collect their entries in sections the
 * linker brackets with __start_ / __stop_ symbols. The entries hold pointers, so the sections are writable, and
 * left to themselves they land among RELRO, where the console's converter puts the process parameters: they go
 * after .data, in the data segment.
 *
 * SPDX-License-Identifier: MIT
 */
SECTIONS {
    resets : { KEEP(*(resets)) } : data
    hle_impls : { KEEP(*(hle_impls)) } : data
    services : { KEEP(*(services)) } : data
} INSERT AFTER .data;
