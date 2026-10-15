// The part of SDL 3 LAS Recomp uses, on the PS5's own services: the runtime is compiled against SDL's own headers
// (ps5/tools/build-deps.sh) and links this in place of the library, which has no PS5 port.
//
//   window      the display, through RADV's VK_KHR_display (SDL_Vulkan_*), at its mode's size
//   gamepad     the DualSense of the user who started the title (ps5/src/console.c), as one SDL gamepad of type PS5:
//               its button and axis events, and its state
//   audio       SDL audio streams, mixed into the console's one 48 kHz output (console.c's sound thread)
//   time        SDL_GetTicks*, SDL_Delay
//   the rest    what has no console counterpart answers as SDL does when a feature is unavailable (no file dialogs, no
//               URLs, no keyboard text input); a message box is a log line
//
// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <volk.h>

#include "../src/console.h"
#include "scancode_names.h"

static char g_error[256] = "";
static bool set_error(const char *what) {
    snprintf(g_error, sizeof g_error, "%s", what);
    return false;
}
const char *SDL_GetError(void) { return g_error; }

void SDL_free(void *mem) { free(mem); }
bool SDL_SetHint(const char *name, const char *value) { (void)name, (void)value; return true; }

// ------------------------------------------------------------------ time
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t g_t0;
Uint64 SDL_GetTicksNS(void) { return now_ns() - g_t0; }
Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000ull; }
void SDL_Delay(Uint32 ms) { usleep(ms * 1000u); }
__attribute__((constructor)) static void ticks_start(void) { g_t0 = now_ns(); }

// The console's CPU: 8 Zen 2 cores with SMT. 16 GB, of which a title is given about 12 (direct memory).
int SDL_GetNumLogicalCPUCores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 8;
}
int SDL_GetSystemRAM(void) { return 12 * 1024; }

static void pad_start(void);
static void audio_init(void);

bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags & SDL_INIT_GAMEPAD) pad_start();
    if (flags & SDL_INIT_AUDIO) audio_init();
    return true;
}

// ------------------------------------------------------------------ the window: the display
struct SDL_Window { int unused; };
static struct SDL_Window g_window;
static int g_w = 3840, g_h = 2160;  // the display mode's, once the surface is made (SDL_Vulkan_CreateSurface)
static SDL_DisplayMode g_mode = {.displayID = 1, .format = SDL_PIXELFORMAT_XRGB8888, .w = 3840, .h = 2160,
                                 .pixel_density = 1, .refresh_rate = 59.94f};

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)title, (void)w, (void)h, (void)flags;
    return &g_window;
}
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    (void)window;
    if (w) *w = g_w;
    if (h) *h = g_h;
    return true;
}
float SDL_GetWindowPixelDensity(SDL_Window *window) { (void)window; return 1; }
bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen) { (void)window, (void)fullscreen; return true; }
bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) { (void)window, (void)icon; return true; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window) { (void)window; return 1; }
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID displayID) { (void)displayID; return &g_mode; }

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags, (void)window;
    fprintf(stderr, "[%s] %s\n", title ? title : "message", message ? message : "");
    return true;
}
bool SDL_OpenURL(const char *url) { (void)url; return set_error("no browser on the PS5"); }

// No file dialogs: answered as cancelled (an empty list), as SDL does when the user closes one
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                              const char *default_location, bool allow_many) {
    (void)window, (void)default_location, (void)allow_many;
    static const char *const none[] = {NULL};
    set_error("no file dialogs on the PS5");
    if (callback) callback(userdata, none, -1);
}
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int nfilters, const char *default_location,
                            bool allow_many) {
    (void)filters, (void)nfilters;
    SDL_ShowOpenFolderDialog(callback, userdata, window, default_location, allow_many);
}
bool SDL_StartTextInput(SDL_Window *window) { (void)window; return true; }
bool SDL_StopTextInput(SDL_Window *window) { (void)window; return true; }

// ------------------------------------------------------------------ Vulkan: RADV, linked in, on the display
// Vulkan's commands come from the RADV linked into the title, through volk (the runtime is compiled with
// VK_NO_PROTOTYPES and volk.h: ps5/CMakeLists.txt). The runtime asks for the instance extensions first and makes
// the surface right after the instance (runtime/nvn/vulkan/device.c), which is where volk is started and loaded.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    static const char *const ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    static bool started;
    if (!started) {
        started = true;
        volkInitializeCustom((PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr);
    }
    if (count) *count = 2;
    return ext;
}

// The display's mode: the largest, at the refresh nearest 60 Hz (the game's own pace; a 120 Hz output needs
// param.json to ask for it, and the WSI measures what the display really does). Its plane and alpha as
// PS5_VulkanTemplate's base/VulkanSwapChain.cpp chooses them.
bool SDL_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, const struct VkAllocationCallbacks *allocator,
                              VkSurfaceKHR *surface) {
    (void)window;
    volkLoadInstance(instance);
    uint32_t n = 1;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(instance, &n, &phys) < 0 || !phys) return set_error("no GPU");
    uint32_t nd = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &nd, NULL);
    VkDisplayPropertiesKHR *dp = calloc(nd ? nd : 1, sizeof *dp);
    vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &nd, dp);
    VkDisplayKHR display = VK_NULL_HANDLE;
    VkDisplayModePropertiesKHR best = {0};
    for (uint32_t i = 0; i < nd; i++) {
        uint32_t nm = 0;
        vkGetDisplayModePropertiesKHR(phys, dp[i].display, &nm, NULL);
        VkDisplayModePropertiesKHR *mp = calloc(nm ? nm : 1, sizeof *mp);
        vkGetDisplayModePropertiesKHR(phys, dp[i].display, &nm, mp);
        for (uint32_t j = 0; j < nm; j++) {
            const VkDisplayModeParametersKHR *a = &mp[j].parameters, *b = &best.parameters;
            uint64_t area_a = (uint64_t)a->visibleRegion.width * a->visibleRegion.height;
            uint64_t area_b = (uint64_t)b->visibleRegion.width * b->visibleRegion.height;
            bool better = area_a > area_b ||
                          (area_a == area_b && abs((int)a->refreshRate - 60000) < abs((int)b->refreshRate - 60000));
            fprintf(stderr, "[sdl-ps5] display %u mode %ux%u @ %.2f Hz\n", i, a->visibleRegion.width,
                    a->visibleRegion.height, a->refreshRate / 1000.0);
            if (!display || better) { best = mp[j]; display = dp[i].display; }
        }
        free(mp);
    }
    free(dp);
    if (!display) return set_error("no display mode");

    uint32_t np = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys, &np, NULL);
    VkDisplayPlanePropertiesKHR *pp = calloc(np ? np : 1, sizeof *pp);
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys, &np, pp);
    uint32_t plane = UINT32_MAX;
    for (uint32_t i = 0; i < np && plane == UINT32_MAX; i++) {
        uint32_t ns = 0;
        vkGetDisplayPlaneSupportedDisplaysKHR(phys, i, &ns, NULL);
        VkDisplayKHR *sd = calloc(ns ? ns : 1, sizeof *sd);
        vkGetDisplayPlaneSupportedDisplaysKHR(phys, i, &ns, sd);
        for (uint32_t j = 0; j < ns; j++)
            if (sd[j] == display) plane = i;
        free(sd);
    }
    if (plane == UINT32_MAX) { free(pp); return set_error("no display plane"); }
    VkDisplayPlaneCapabilitiesKHR cap = {0};
    vkGetDisplayPlaneCapabilitiesKHR(phys, best.displayMode, plane, &cap);
    VkDisplayPlaneAlphaFlagBitsKHR alpha =
        cap.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR ? VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR
        : cap.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR             ? VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR
        : cap.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR                ? VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR
                                                                                    : VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    VkDisplaySurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
                                        .displayMode = best.displayMode,
                                        .planeIndex = plane,
                                        .planeStackIndex = pp[plane].currentStackIndex,
                                        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                        .globalAlpha = 1,
                                        .alphaMode = alpha,
                                        .imageExtent = best.parameters.visibleRegion};
    free(pp);
    VkResult r = vkCreateDisplayPlaneSurfaceKHR(instance, &si, allocator, surface);
    if (r != VK_SUCCESS) return set_error("vkCreateDisplayPlaneSurfaceKHR failed");
    g_w = g_mode.w = (int)best.parameters.visibleRegion.width;
    g_h = g_mode.h = (int)best.parameters.visibleRegion.height;
    g_mode.refresh_rate = best.parameters.refreshRate / 1000.0f;
    g_mode.refresh_rate_numerator = (int)best.parameters.refreshRate;
    g_mode.refresh_rate_denominator = 1000;
    fprintf(stderr, "[sdl-ps5] display: %dx%d @ %.2f Hz, plane %u\n", g_w, g_h, g_mode.refresh_rate, plane);
    return true;
}

// ------------------------------------------------------------------ events
#define QUEUE 512
static SDL_Event g_queue[QUEUE];
static int g_qhead, g_qcount;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

bool SDL_PushEvent(SDL_Event *e) {
    pthread_mutex_lock(&g_qlock);
    bool ok = g_qcount < QUEUE;
    if (ok) {
        g_queue[(g_qhead + g_qcount) % QUEUE] = *e;
        if (!g_queue[(g_qhead + g_qcount) % QUEUE].common.timestamp)
            g_queue[(g_qhead + g_qcount) % QUEUE].common.timestamp = SDL_GetTicksNS();
        g_qcount++;
    }
    pthread_mutex_unlock(&g_qlock);
    return ok;
}

// (e NULL: only whether there is one, as SDL_PollEvent(NULL))
static bool pop(SDL_Event *e) {
    pthread_mutex_lock(&g_qlock);
    bool ok = g_qcount > 0;
    if (ok && e) {
        *e = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QUEUE;
        g_qcount--;
    }
    pthread_mutex_unlock(&g_qlock);
    return ok;
}

static void pad_pump(void);

bool SDL_PollEvent(SDL_Event *e) {
    pad_pump();
    return pop(e);
}

bool SDL_WaitEventTimeout(SDL_Event *e, Sint32 timeoutMS) {
    uint64_t end = now_ns() + (uint64_t)(timeoutMS < 0 ? 0 : timeoutMS) * 1000000ull;
    for (;;) {
        if (SDL_PollEvent(e)) return true;
        if (timeoutMS >= 0 && now_ns() >= end) return false;
        usleep(1000);
    }
}

// ------------------------------------------------------------------ the gamepad
// One gamepad, id 1: the starting user's DualSense (console.c's player 0). Its readings become SDL's events and
// state, as SDL's own PS5 driver reports them: cross is SOUTH, circle EAST, square WEST, triangle NORTH, OPTIONS
// START. The touch pad's click is BACK (SDL's TOUCHPAD too): the game's Minus, and with OPTIONS the overlay menu
// (the Create button stays the system's).
#define PAD_ID 1
struct SDL_Gamepad { SDL_JoystickID id; };
static struct SDL_Gamepad g_pad = {PAD_ID};
static bool g_pad_started, g_pad_open, g_pad_announced;
static bool g_buttons[SDL_GAMEPAD_BUTTON_COUNT];
static Sint16 g_axes[SDL_GAMEPAD_AXIS_COUNT];

static const struct { uint32_t ps5; SDL_GamepadButton sdl; } BUTTONS[] = {
    {PAD_CROSS, SDL_GAMEPAD_BUTTON_SOUTH},       {PAD_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
    {PAD_SQUARE, SDL_GAMEPAD_BUTTON_WEST},       {PAD_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
    {PAD_OPTIONS, SDL_GAMEPAD_BUTTON_START},     {PAD_TOUCH_PAD, SDL_GAMEPAD_BUTTON_BACK},
    {PAD_TOUCH_PAD, SDL_GAMEPAD_BUTTON_TOUCHPAD}, {PAD_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {PAD_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},    {PAD_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {PAD_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {PAD_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {PAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},    {PAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {PAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

static void pad_start(void) {
    if (g_pad_started) return;
    g_pad_started = true;
    g_pad_open = pad_open();
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) g_axes[i] = 0;
}

static Sint16 stick_axis(uint8_t v) {
    int a = ((int)v - 128) * 256;
    return (Sint16)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
}
static Sint16 trigger_axis(uint8_t v) { return (Sint16)(v * 32767 / 255); }

static void push_axis(SDL_GamepadAxis axis, Sint16 v) {
    if (g_axes[axis] == v) return;
    g_axes[axis] = v;
    SDL_Event e = {0};
    e.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = PAD_ID;
    e.gaxis.axis = (Uint8)axis;
    e.gaxis.value = v;
    SDL_PushEvent(&e);
}

// Every reading since the last pump, oldest first, so a press shorter than a frame is still a down and an up
static void pad_pump(void) {
    if (!g_pad_open) return;
    static uint64_t last;
    uint64_t t = now_ns();
    if (t - last < 1000000ull) return;  // (the pad reports about every 4 ms; SDL_WaitEventTimeout pumps each ms)
    last = t;
    if (!g_pad_announced) {
        g_pad_announced = true;
        SDL_Event e = {0};
        e.gdevice.type = SDL_EVENT_GAMEPAD_ADDED;
        e.gdevice.which = PAD_ID;
        SDL_PushEvent(&e);
    }
    struct pad p;
    pad_poll(&p);
    const struct pad_reading *r;
    int n = pad_readings(&r);
    for (int i = 0; i < n; i++) {
        if (!r[i].connected || (r[i].buttons & PAD_INTERCEPTED)) continue;  // (the shell has the pad)
        for (size_t b = 0; b < sizeof BUTTONS / sizeof *BUTTONS; b++) {
            bool down = (r[i].buttons & BUTTONS[b].ps5) != 0;
            if (down == g_buttons[BUTTONS[b].sdl]) continue;
            g_buttons[BUTTONS[b].sdl] = down;
            SDL_Event e = {0};
            e.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            e.gbutton.which = PAD_ID;
            e.gbutton.button = (Uint8)BUTTONS[b].sdl;
            e.gbutton.down = down;
            SDL_PushEvent(&e);
        }
        push_axis(SDL_GAMEPAD_AXIS_LEFTX, stick_axis(r[i].left_x));
        push_axis(SDL_GAMEPAD_AXIS_LEFTY, stick_axis(r[i].left_y));
        push_axis(SDL_GAMEPAD_AXIS_RIGHTX, stick_axis(r[i].right_x));
        push_axis(SDL_GAMEPAD_AXIS_RIGHTY, stick_axis(r[i].right_y));
        push_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger_axis(r[i].l2));
        push_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger_axis(r[i].r2));
    }
}

SDL_JoystickID *SDL_GetGamepads(int *count) {
    int n = g_pad_open ? 1 : 0;
    SDL_JoystickID *ids = calloc(2, sizeof *ids);
    if (ids && n) ids[0] = PAD_ID;
    if (count) *count = n;
    return ids;
}
bool SDL_HasGamepad(void) { return g_pad_open; }
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) { return id == PAD_ID && g_pad_open ? &g_pad : NULL; }
void SDL_CloseGamepad(SDL_Gamepad *gamepad) { (void)gamepad; }
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad) { return gamepad ? "DualSense Wireless Controller" : NULL; }
const char *SDL_GetGamepadNameForID(SDL_JoystickID id) { return id == PAD_ID ? SDL_GetGamepadName(&g_pad) : NULL; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) {
    return gamepad ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID id) {
    return id == PAD_ID ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad) { return gamepad ? gamepad->id : 0; }
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) {
    return gamepad && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && g_buttons[button];
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    return gamepad && axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT ? g_axes[axis] : 0;
}

// ------------------------------------------------------------------ keyboard names (key bindings in larecomp.ini)
// SDL's default US keymap: letters and digits are their characters, Return, Escape, Backspace, Tab and Space theirs,
// every other key its scancode's keycode.
SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        if (SDL_scancode_names[i] && !strcasecmp(name, SDL_scancode_names[i])) return (SDL_Scancode)i;
    return SDL_SCANCODE_UNKNOWN;
}
const char *SDL_GetScancodeName(SDL_Scancode sc) {
    return sc >= 0 && sc < SDL_SCANCODE_COUNT && SDL_scancode_names[sc] ? SDL_scancode_names[sc] : "";
}
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc, SDL_Keymod mod, bool key_event) {
    (void)key_event;
    bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (SDL_Keycode)((shift ? 'A' : 'a') + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (SDL_Keycode)('1' + (sc - SDL_SCANCODE_1));
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: return SDLK_RETURN;
    case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
    case SDL_SCANCODE_TAB: return SDLK_TAB;
    case SDL_SCANCODE_SPACE: return SDLK_SPACE;
    default: return sc > SDL_SCANCODE_UNKNOWN && sc < SDL_SCANCODE_COUNT ? SDL_SCANCODE_TO_KEYCODE(sc) : SDLK_UNKNOWN;
    }
}
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *mod) {
    if (mod) *mod = SDL_KMOD_NONE;
    if (key & SDLK_SCANCODE_MASK) return (SDL_Scancode)(key & ~SDLK_SCANCODE_MASK);
    if (key >= 'A' && key <= 'Z') { if (mod) *mod = SDL_KMOD_LSHIFT; return (SDL_Scancode)(SDL_SCANCODE_A + (key - 'A')); }
    if (key >= 'a' && key <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (key - 'a'));
    if (key >= '1' && key <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (key - '1'));
    switch (key) {
    case '0': return SDL_SCANCODE_0;
    case SDLK_RETURN: return SDL_SCANCODE_RETURN;
    case SDLK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case SDLK_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case SDLK_TAB: return SDL_SCANCODE_TAB;
    case SDLK_SPACE: return SDL_SCANCODE_SPACE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

// ------------------------------------------------------------------ surfaces: screenshots
// The window icon has nothing to show on; screenshots (runtime/nvn/vulkan/device.c) are saved as PNG with stored
// (uncompressed) deflate blocks: no zlib in the title, and a test run's picture is read on the PC.
SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    SDL_Surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->format = format;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->pixels = pixels;
    s->refcount = 1;
    return s;
}
void SDL_DestroySurface(SDL_Surface *surface) { free(surface); }
SDL_Surface *SDL_LoadPNG(const char *file) { (void)file; set_error("no PNG loading on the PS5"); return NULL; }

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    uint32_t c = crc(crc(0, h + 4, 4), data, n);
    uint8_t t[4];
    be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (!n || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4;
}

bool SDL_SavePNG(SDL_Surface *s, const char *file) {
    if (!s || s->format != SDL_PIXELFORMAT_ABGR8888) return set_error("SDL_SavePNG: RGBA8 surfaces only");
    size_t row = (size_t)s->w * 4 + 1, raw = row * (size_t)s->h, blocks = (raw + 65534) / 65535;
    size_t zn = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(zn), *line = malloc(row);
    FILE *f = z && line ? fopen(file, "wb") : NULL;
    if (!f) { free(z); free(line); return set_error("SDL_SavePNG: can't write the file"); }
    uint8_t *o = z;
    *o++ = 0x78, *o++ = 0x01;
    uint32_t a = 1, b = 0;  // adler-32
    size_t y = 0, x = row;
    for (size_t done = 0; done < raw;) {
        size_t n = raw - done < 65535 ? raw - done : 65535;
        *o++ = done + n == raw;
        *o++ = (uint8_t)n, *o++ = (uint8_t)(n >> 8), *o++ = (uint8_t)~n, *o++ = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++) {
            if (x == row) {  // next row: filter byte 0, then its pixels
                line[0] = 0;
                memcpy(line + 1, (const uint8_t *)s->pixels + y++ * (size_t)s->pitch, row - 1);
                x = 0;
            }
            uint8_t v = line[x++];
            *o++ = v;
            a = (a + v) % 65521, b = (b + a) % 65521;
        }
        done += n;
    }
    be32(o, (b << 16) | a);
    o += 4;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)s->w);
    be32(ihdr + 4, (uint32_t)s->h);
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;  // 8-bit RGBA
    bool ok = fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)(o - z)) && chunk(f, "IEND", NULL, 0);
    ok &= fclose(f) == 0;
    free(z);
    free(line);
    return ok || set_error("SDL_SavePNG: write failed");
}

// ------------------------------------------------------------------ audio
// Each stream converts what it is given to 48 kHz float stereo (linear interpolation; 6 channels mixed down as
// SDL's default does it, mono doubled) into a ring the sound thread drains, 256 frames at a time, mixing every
// stream. Queued bytes are what a stream holds, in its own format, as SDL counts them.
#define RATE 48000
#define RING (RATE * 2)  // frames: two seconds
struct SDL_AudioStream {
    SDL_AudioSpec spec;
    float gain;
    float ring[RING][2];
    int head, count;
    double t;          // resampling: position between prev and the next input frame
    float prev[2];
    struct SDL_AudioStream *next;
};
static pthread_mutex_t g_alock = PTHREAD_MUTEX_INITIALIZER;
static struct SDL_AudioStream *g_streams;
static bool g_audio_tried, g_audio_out;

static void mix(int16_t *frames, int count, void *user) {
    (void)user;
    float acc[256][2];
    memset(acc, 0, sizeof acc);
    if (count > 256) count = 256;
    pthread_mutex_lock(&g_alock);
    for (struct SDL_AudioStream *s = g_streams; s; s = s->next) {
        int n = s->count < count ? s->count : count;
        for (int i = 0; i < n; i++) {
            acc[i][0] += s->ring[s->head][0] * s->gain;
            acc[i][1] += s->ring[s->head][1] * s->gain;
            s->head = (s->head + 1) % RING;
        }
        s->count -= n;
    }
    pthread_mutex_unlock(&g_alock);
    for (int i = 0; i < count; i++)
        for (int c = 0; c < 2; c++) {
            float v = acc[i][c] * 32767.0f;
            frames[i * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)lrintf(v));
        }
}

static void audio_init(void) {
    if (g_audio_tried) return;
    g_audio_tried = true;
    g_audio_out = audio_start(mix, NULL);
    if (!g_audio_out) fprintf(stderr, "[sdl-ps5] no sound output\n");
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                           SDL_AudioStreamCallback callback, void *userdata) {
    (void)devid, (void)userdata;
    if (callback) { set_error("audio stream callbacks are not supported"); return NULL; }
    if (!spec || (spec->format != SDL_AUDIO_S16 && spec->format != SDL_AUDIO_F32) || spec->channels < 1 ||
        spec->channels > 8 || spec->freq <= 0) {
        set_error("unsupported audio format");
        return NULL;
    }
    audio_init();
    SDL_AudioStream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->spec = *spec;
    s->gain = 1;
    pthread_mutex_lock(&g_alock);
    s->next = g_streams;
    g_streams = s;
    pthread_mutex_unlock(&g_alock);
    return s;
}

void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    pthread_mutex_lock(&g_alock);
    for (SDL_AudioStream **p = &g_streams; *p; p = &(*p)->next)
        if (*p == s) { *p = s->next; break; }
    pthread_mutex_unlock(&g_alock);
    free(s);
}

static void in_frame(const SDL_AudioStream *s, const uint8_t *p, float out[2]) {
    float ch[8];
    int n = s->spec.channels;
    for (int c = 0; c < n; c++)
        ch[c] = s->spec.format == SDL_AUDIO_F32 ? ((const float *)p)[c] : ((const int16_t *)p)[c] / 32768.0f;
    if (n == 1) { out[0] = out[1] = ch[0]; return; }
    out[0] = ch[0], out[1] = ch[1];
    if (n >= 6) {  // FL FR FC LFE BL BR (+ SL SR): centre and backs mixed in at -3 dB, LFE left out
        const float k = 0.7071f;
        out[0] += k * ch[2] + k * ch[4];
        out[1] += k * ch[2] + k * ch[5];
        if (n == 8) out[0] += k * ch[6], out[1] += k * ch[7];
    } else if (n >= 3) {
        out[0] += 0.7071f * ch[2], out[1] += 0.7071f * ch[2];
    }
}

static void ring_push(SDL_AudioStream *s, const float v[2]) {
    if (s->count == RING) { s->head = (s->head + 1) % RING; s->count--; }  // (full: the oldest goes)
    int at = (s->head + s->count) % RING;
    s->ring[at][0] = v[0], s->ring[at][1] = v[1];
    s->count++;
}

bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    if (!s || !buf || len < 0) return set_error("bad audio data");
    if (!g_audio_out) return true;  // (nothing plays it: as if played at once)
    int fb = SDL_AUDIO_FRAMESIZE(s->spec), n = len / fb;
    double step = (double)s->spec.freq / RATE;
    pthread_mutex_lock(&g_alock);
    for (int i = 0; i < n; i++) {
        float x[2];
        in_frame(s, (const uint8_t *)buf + (size_t)i * fb, x);
        while (s->t <= 1.0) {
            float t = (float)s->t, o[2] = {s->prev[0] + (x[0] - s->prev[0]) * t, s->prev[1] + (x[1] - s->prev[1]) * t};
            ring_push(s, o);
            s->t += step;
        }
        s->t -= 1.0;
        s->prev[0] = x[0], s->prev[1] = x[1];
    }
    pthread_mutex_unlock(&g_alock);
    return true;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream *s) {
    if (!s) return -1;
    pthread_mutex_lock(&g_alock);
    double frames = (double)s->count * s->spec.freq / RATE;
    pthread_mutex_unlock(&g_alock);
    return (int)frames * SDL_AUDIO_FRAMESIZE(s->spec);
}

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s) { return s ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : 0; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sample_frames) {
    (void)devid;
    if (spec) *spec = (SDL_AudioSpec){SDL_AUDIO_F32, 2, RATE};
    if (sample_frames) *sample_frames = 256;  // (console.c's grain)
    return true;
}
bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain) {
    if (!s || gain < 0) return set_error("bad gain");
    s->gain = gain;
    return true;
}
float SDL_GetAudioStreamGain(SDL_AudioStream *s) { return s ? s->gain : -1; }
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { return s != NULL; }
