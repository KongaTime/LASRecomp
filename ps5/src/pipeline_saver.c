// The Vulkan pipeline cache on the PS5 (cache/vk_pipelines.bin). The desktop saves it when the app quits
// (runtime/nvn/vulkan/device.c's exit handler), but a PS5 title is closed from the home screen and ends there and
// then, so it never would be: every launch would compile its pipelines again, and their textures show black
// meanwhile. A thread of its own saves it once a minute while it has grown. (The SPIR-V cache, cache/spirv/, is
// written shader by shader as the desktop does.)
//
// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "nvn/vulkan/vk.h"

void vk_pipeline_cache_save(void);

static void *saver(void *arg) {
    (void)arg;
    size_t saved = 0;
    for (;;) {
        sleep(60);
        size_t sz = 0;
        if (!g_vk.dev || !g_vk.pcache || vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS)
            continue;
        if (sz <= saved) continue;
        vk_pipeline_cache_save();
        fprintf(stderr, "[ps5] pipeline cache saved (%zu KB)\n", sz >> 10);
        saved = sz;
    }
    return NULL;
}

void ps5_pipeline_saver_start(void) {
    static int started;
    if (started++) return;
    pthread_t t;
    if (pthread_create(&t, NULL, saver, NULL) == 0) pthread_detach(t);
}
