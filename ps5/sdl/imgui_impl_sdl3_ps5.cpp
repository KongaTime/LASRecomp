// Dear ImGui's SDL 3 platform backend (third_party/imgui/backends/imgui_impl_sdl3.h), the part the start screen uses
// (runtime/ui/launcher.mm), for the PS5: the display's size and the frame time. The controller reaches ImGui through
// the start screen's own gamepad handling (launcher_pad_event, manual gamepad mode), from ps5/sdl/sdl_ps5.c's
// events; there is no mouse, keyboard, clipboard or cursor to report.
//
// SPDX-License-Identifier: MIT
#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

static Uint64 g_last;

static bool init(SDL_Window *) {
    ImGuiIO &io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_sdl3_ps5";
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_last = 0;
    return true;
}

bool ImGui_ImplSDL3_InitForVulkan(SDL_Window *window) { return init(window); }
bool ImGui_ImplSDL3_InitForOther(SDL_Window *window) { return init(window); }

void ImGui_ImplSDL3_Shutdown() {
    ImGuiIO &io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
}

void ImGui_ImplSDL3_NewFrame() {
    ImGuiIO &io = ImGui::GetIO();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nullptr, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    Uint64 now = SDL_GetTicksNS();
    io.DeltaTime = g_last && now > g_last ? (float)((double)(now - g_last) / 1e9) : 1.0f / 60;
    g_last = now;
}

bool ImGui_ImplSDL3_ProcessEvent(const SDL_Event *) { return false; }

void ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode, SDL_Gamepad **, int) {}
