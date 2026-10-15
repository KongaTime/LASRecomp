# LAS Recomp on the PS5 - the title's build (ps5/tools/build.sh drives it).
#
# Configured with the payload SDK's toolchain (toolchain/prospero.cmake), as PS5_VulkanTemplate builds its titles. It
# compiles what the Makefile's Linux build does - the runtime, its Vulkan backend, the start screen and the overlay
# (runtime/ui/*.mm, as C++), Dear ImGui - with the PS5's parts in place of the desktop's:
#
#   runtime/platform/ps5.c     the host layer (platform.h): guest memory in direct memory, the log, crashes, the exit
#   runtime/platform/ps5_elf.c the game code library's loader (no dlopen on the console)
#   ps5/sdl/                   the part of SDL 3 the runtime uses, on the console's display, pad and sound
#   ps5/src/                   the console layer (PS5_VulkanTemplate's), the exit wraps, the import table
#
# against the libraries ps5/tools/build-deps.sh builds (build/ps5-deps: shaderc, FFmpeg, SDL's headers, Vulkan-Headers,
# volk, VMA), then ps5/tools/link-title.sh links them with RADV and lays out the title folder. No game code: the title
# loads lasgame.ps5.so, built on a PC from the player's own game files (ps5/tools/gamelib.py).
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(lasrecomp_ps5 C CXX)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(PS5_VULKAN_DIR "${ROOT}/../PS5_Vulkan" CACHE PATH "the PS5_Vulkan checkout (RADV archive, link recipe, packaging)")
set(PS5_DEPS "${ROOT}/build/ps5-deps" CACHE PATH "ps5/tools/build-deps.sh's folder")
set(DEPS ${PS5_DEPS}/prefix)
if(NOT EXISTS ${DEPS}/lib/libshaderc_combined.a OR NOT EXISTS ${DEPS}/lib/libavcodec.a)
	message(FATAL_ERROR "No PS5 libraries in ${DEPS}: run ps5/tools/build-deps.sh first")
endif()

# The game code's id: what lasgame.ps5.so must have been built with (ps5/tools/gamelib.py, the Makefile's GAMELIB_ID)
execute_process(COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gamelib.py id OUTPUT_VARIABLE GAMELIB_ID
	OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gamelib.py
	${ROOT}/tools/gen.py ${ROOT}/tools/lifter.py ${ROOT}/tools/analyze.py ${ROOT}/tools/nso.py ${ROOT}/tools/passes.py
	${ROOT}/tools/patches.py ${ROOT}/tools/simd_c.py ${ROOT}/tools/rando/patch_variants.json ${ROOT}/tools/gamelib_entry.c
	${ROOT}/runtime/hle/hle_symbols.txt ${ROOT}/runtime/cpu.h ${ROOT}/runtime/simd.h ${ROOT}/runtime/simd_fp.h
	${ROOT}/runtime/gamelib.h)
message(STATUS "LAS Recomp: game code id ${GAMELIB_ID}")

# --- the runtime (the Makefile's RT_SRC, BACKEND_SRC, UI_SRC, IMGUI_OBJ) ------------------------------------------
file(GLOB rt_sources ${ROOT}/runtime/*.c ${ROOT}/runtime/hle/*.c ${ROOT}/runtime/services/*.c ${ROOT}/runtime/nvn/*.c
	${ROOT}/runtime/nvn/maxwell/*.c)
list(APPEND rt_sources ${ROOT}/runtime/platform/ps5.c ${ROOT}/runtime/platform/ps5_elf.c)
file(GLOB backend_sources ${ROOT}/runtime/nvn/vulkan/*.c ${ROOT}/runtime/nvn/vulkan/*.cpp)
file(GLOB ui_sources ${ROOT}/runtime/ui/*.mm)
set(imgui ${ROOT}/third_party/imgui)
set(imgui_sources ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp
	${imgui}/backends/imgui_impl_vulkan.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sdl/imgui_impl_sdl3_ps5.cpp)
set(ps5_sources ${CMAKE_CURRENT_SOURCE_DIR}/sdl/sdl_ps5.c ${CMAKE_CURRENT_SOURCE_DIR}/src/console.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/title.c ${DEPS}/volk/volk.c)

# SMAA's reference shader as a C string (the Makefile's smaa_hlsl.inc rule)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/inc/smaa_hlsl.inc
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/inc
	COMMAND sh -c "LC_ALL=C tr -d '\\r' < '${ROOT}/third_party/smaa/SMAA.hlsl' | LC_ALL=C tr '\\200-\\377' '?' | sed -e 's/\\\\/\\\\\\\\/g' -e 's/\"/\\\\\"/g' -e 's/^/\"/' -e 's/$/\\\\n\"/' > '${CMAKE_BINARY_DIR}/inc/smaa_hlsl.inc'"
	DEPENDS ${ROOT}/third_party/smaa/SMAA.hlsl VERBATIM)
set_source_files_properties(${ROOT}/runtime/nvn/smaa_data.c PROPERTIES
	OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/inc/smaa_hlsl.inc
	INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/inc;${ROOT}/third_party/smaa/Textures")

add_library(lasrt OBJECT ${rt_sources} ${backend_sources} ${ui_sources} ${imgui_sources} ${ps5_sources})
target_include_directories(lasrt PRIVATE ${ROOT}/runtime ${ROOT}/runtime/nvn ${imgui} ${imgui}/backends
	${DEPS}/include ${DEPS}/volk)
# Every unit sees volk's declarations first: the Vulkan commands are volk's pointers, filled from the RADV linked
# into the title (ps5/sdl/sdl_ps5.c starts it); ImGui's Vulkan backend takes them from volk too. ImGui's own
# open-in-shell helper (fork / exec) has nothing to start on the console.
target_compile_definitions(lasrt PRIVATE VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
	"GAMELIB_ID=\"${GAMELIB_ID}\""
	"GAMELIB_CFLAGS=\"ps5/tools/gamelib.py\"")
# -fno-omit-frame-pointer: the console's crash backtraces are frame-pointer walks (runtime/platform/ps5.c).
# -march=znver2: the console's CPU (Zen 2).
set(common_options -O2 -g1 -march=znver2 -fno-omit-frame-pointer -fno-strict-aliasing
	"SHELL:-include ${DEPS}/volk/volk.h")
target_compile_options(lasrt PRIVATE ${common_options}
	$<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-function -Wno-format>
	$<$<COMPILE_LANGUAGE:CXX>:-Wno-format -Wno-nullability-completeness>)
set_target_properties(lasrt PROPERTIES C_STANDARD 11 C_EXTENSIONS ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# The start screen and the overlay are plain C++ apart from __APPLE__ blocks (the Makefile's UIFLAGS)
set_source_files_properties(${ui_sources} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${DEPS}/volk/volk.c PROPERTIES COMPILE_OPTIONS "-w")

# --- the import table: what lasgame.ps5.so may call (runtime/platform/ps5_elf.h) ---------------------------------
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gamelib_imports.c
	COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/imports.py ${PS5_PAYLOAD_SDK}/bin/llvm-nm
		${CMAKE_BINARY_DIR}/gamelib_imports.c "$<TARGET_OBJECTS:lasrt>"
	DEPENDS lasrt "$<TARGET_OBJECTS:lasrt>" ${CMAKE_CURRENT_SOURCE_DIR}/tools/imports.py
	COMMAND_EXPAND_LISTS VERBATIM)
add_library(lasimports OBJECT ${CMAKE_BINARY_DIR}/gamelib_imports.c)
target_include_directories(lasimports PRIVATE ${ROOT}/runtime)
target_compile_options(lasimports PRIVATE -O2 -fno-builtin -w)

# --- link, sign and lay out: build/ps5/dist/<TITLE_ID>/ ----------------------------------------------------------
set(libs ${DEPS}/lib/libshaderc_combined.a ${DEPS}/lib/libavformat.a ${DEPS}/lib/libavcodec.a ${DEPS}/lib/libswscale.a
	${DEPS}/lib/libavutil.a)
add_custom_command(
	OUTPUT ${CMAKE_BINARY_DIR}/eboot.elf
	COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/link-title.sh ${CMAKE_BINARY_DIR} ${PS5_VULKAN_DIR} ${PS5_PAYLOAD_SDK}
		"$<TARGET_OBJECTS:lasrt>" "$<TARGET_OBJECTS:lasimports>" ${libs}
	DEPENDS lasrt lasimports "$<TARGET_OBJECTS:lasrt>" "$<TARGET_OBJECTS:lasimports>" ${libs}
		${CMAKE_CURRENT_SOURCE_DIR}/tools/link-title.sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/stage-notices.py
		${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/param.json ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png
	COMMAND_EXPAND_LISTS
	VERBATIM)
add_custom_target(title ALL DEPENDS ${CMAKE_BINARY_DIR}/eboot.elf)
