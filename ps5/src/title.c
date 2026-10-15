// The title's ending on the PS5: the shell closes it (runtime/platform/ps5.c, plat_terminate). exit() is reported by
// the console as a crash (SIGSYS in libkernel), so the link wraps it (ps5/tools/link-title.sh: --wrap=exit and the
// others below): the runtime's own exits (the start screen's quit, fatal errors after their report, the test
// switches) run their atexit handlers as before (the Vulkan backend saves its pipeline cache in one), then ask the
// shell. libc's own atexit list can't be walked from here, so atexit is wrapped too and keeps them itself.
//
// SPDX-License-Identifier: MIT
#include <stdio.h>

void plat_terminate(void) __attribute__((noreturn));

#define MAX_HANDLERS 32
static void (*g_handlers[MAX_HANDLERS])(void);
static int g_count;

int __wrap_atexit(void (*fn)(void)) {
    if (g_count == MAX_HANDLERS) return -1;
    g_handlers[g_count++] = fn;
    return 0;
}

void __wrap_exit(int status) {
    fprintf(stderr, "[title] exit(%d): closing through the shell\n", status);
    while (g_count > 0) g_handlers[--g_count]();  // (in reverse, as exit runs them)
    plat_terminate();
}

void __wrap__exit(int status) {
    fprintf(stderr, "[title] _exit(%d): closing through the shell\n", status);
    plat_terminate();
}

void __wrap__Exit(int status) { __wrap__exit(status); }
