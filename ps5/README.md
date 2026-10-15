# LAS Recomp on the PS5

LAS Recomp as a native homebrew title for jailbroken PS5 consoles: the same runtime as the Windows, macOS and Linux
builds, with the recompiled game code built on a PC from your own game files, rendering through Vulkan on RADV and
playing with the DualSense.

> [!WARNING]
> **Not yet run on a console.** The title builds, links and packages (a signed `eboot.bin` and its folder), and the
> game code library's path from the PC to the console's loader is tested on a PC (`ps5/tests/`). Nothing here has
> been launched on a PS5 yet. The first runs will find what only the console can (see "First runs"), and the log
> they leave says where it stopped.

No game files are included, and no game code: the title loads the game code you build from your own copy.

## How it works

The desktop builds already split cleanly between the game (the recompiled code, built from the player's files) and
the runtime (everything else). The PS5 port keeps that split and replaces only what the console doesn't have:

| The desktop runtime uses | On the PS5 |
| --- | --- |
| a window, a controller, sound and timers from SDL 3 | `ps5/sdl/sdl_ps5.c`: the SDL calls the runtime makes, against SDL's own headers, on the console's display (RADV's `VK_KHR_display`), its pad and its sound. The DualSense is an SDL gamepad of type PS5: PlayStation button prompts, and the start screen works with it |
| the system's Vulkan driver | RADV, Mesa's Vulkan driver, linked into the title (PS5_Mesa, built by PS5_Vulkan), through volk |
| `dlopen` of the game code library | `runtime/platform/ps5_elf.c`: the title loads the library itself into executable direct memory and binds its imports by name to its own functions (`ps5/tools/imports.py` makes the table) |
| the game code built at setup on the player's machine | `ps5/tools/gamelib.py` on a PC: the same generator (`tools/gen.py`), compiled for the console's CPU with any clang, against nothing of the console |
| a 12.5 GB guest window, reserved and backed lazily | `runtime/platform/ps5.c`: the same window at the same address (`ps5_vrange_reserve_at`), backed by direct memory as it is used (64 KB units, committed by the loader and the kernel, and on first touch by the fault handler); guest threads' 256 MB host stacks the same way |
| `exit()`, a log on stderr, crash backtraces | the shell exit (`ps5/src/title.c` wraps `exit`), `lasrecomp.log` in the title folder and klog, frame-pointer backtraces |
| shaderc and FFmpeg as system libraries | the same versions as the Linux build, cross-built for the console as static libraries (`ps5/tools/build-deps.sh`) |

What changed in the runtime itself is small and behind `__PROSPERO__` (the PS5 compiler's define) or `__ELF__`:
the data root (`/app0`), the game code library's name and loader, guest threads' stacks, the start screen's setup
page (the console can't recompile the game), and the section registries' ELF branch.

## Building the title

On Linux (built on Ubuntu 24.04), beside this repository's folder:

```bash
sudo apt install git cmake ninja-build clang lld python3-pip glslang-tools zstd flex bison nasm wget unzip curl \
    llvm-18-dev libclang-18-dev libclang-cpp18-dev libllvmspirvlib-18-dev libclc-18-dev libclc-18 libclang-rt-18-dev
python3 -m pip install meson mako
ps5/tools/bootstrap.sh      # clones PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK beside this repository and builds them
ps5/tools/build.sh          # the title: build/ps5/dist/PPSA99450/
```

The first `bootstrap.sh` builds RADV (a long build); after that, `build.sh` builds only what changed. The title
folder holds `eboot.bin`, `sce_sys/`, `sce_module/libc.prx`, the start screen's font and the notices.

| Path | |
| --- | --- |
| `ps5/CMakeLists.txt` | the title's build: the runtime as the Makefile's Linux build has it, with the PS5 parts |
| `ps5/sdl/` | the SDL 3 calls the runtime makes, on the console; ImGui's SDL 3 platform backend for the start screen |
| `ps5/src/` | the console layer (PS5_VulkanTemplate's pad, sound and shell exit), the exit wraps |
| `ps5/gamelib/include/` | the few C library declarations the game code is compiled against |
| `ps5/tools/` | bootstrap, build, deps, SDK pin, link, notices, game code (`gamelib.py`), deploy |
| `ps5/tests/` | the game code library's build and load, on a PC |
| `runtime/platform/ps5.c`, `ps5_elf.c` | the host layer and the game code loader |

## Preparing the game (on a PC)

The console can't run the generator or a compiler, and it writes slowly past its first GB or so, so the game is set
up on a PC and copied over:

1. **Set up the game files** as for the desktop: LAS Recomp's start screen (Windows, macOS or Linux), "Set up game
   files", from your own dump of Link's Awakening v1.0.0. It leaves `extracted/` in its data folder (`exefs/`,
   `romfs/`, `romfs.bin`).
2. **Build the game code for the PS5** from this repository, at the same commit as the title (the title refuses a
   library built for another):

   ```bash
   python3 ps5/tools/gamelib.py build <data folder>/extracted
   ```

   It writes `extracted/lasgame.ps5.so` (and its `.id`). It needs clang 18 or later and lld (Linux, macOS with
   Homebrew's llvm and lld, or WSL); the generator's Python packages go into `build/ps5-gamelib/venv`.
3. **Copy `extracted/`** to `/data/homebrew/PPSA99450/extracted/` on the console with an FTP client (the console's
   FTP server is not held to the title's write speed limit). The desktop's own game code library in there
   (`lasgame.so`, `.dll` or `.dylib`) isn't needed.

Randomizer seeds are generated by a helper program the console can't run: make them with the desktop app and copy
its `rando/<seed>/` folders to `/data/homebrew/PPSA99450/rando/`. Switch fonts you imported on the desktop go in
`fonts/` the same way.

## Installing on the console

With a homebrew enabler (etaHEN), ShadowMountPlus (titles in `/data/homebrew` on the home screen) and an FTP server:

```bash
PS5_HOST=<console address> ps5/tools/deploy.sh    # or copy build/ps5/dist/PPSA99450/ to /data/homebrew/
```

Then copy the game (above), and launch **LAS Recomp** from the home screen. Saves, settings (`larecomp.ini`), the
shader caches and `lasrecomp.log` are kept in the title folder; `deploy.sh` never touches them.

## Controls

The DualSense as the Switch's controller, in the positions SDL gives a PlayStation pad: cross is B, circle A, and so
on, remappable in Settings > Controls. **The touch pad click is Minus**, and **touch pad + OPTIONS** opens LAS
Recomp's overlay menu (Minus + Plus on the desktop). The Create button stays the console's.

## First runs

What to look for in `/data/homebrew/PPSA99450/lasrecomp.log` (the run before is `lasrecomp.previous.log`; klog
shows the same lines live):

- `[sdl-ps5] display: 3840x2160 @ 59.94 Hz` - the display, through RADV.
- `[paths] data: /app0` - the data root (`chdir` into the title folder).
- `[gamelib] /app0/extracted/lasgame.ps5.so: N MB at ...` - the game code loaded and bound.
- the guest window: a `FATAL: guest address range ... is not free` means the 12.5 GB reservation at
  0x70_0000_0000 was refused.
- a crash: `*** signal N accessing ...` and a backtrace (match the pcs against `build/ps5/link/llvm-pie.elf`).

Things only the console can answer, in the order a run meets them: the guest window's reservation; backing guest
memory and guest stacks on first touch from the fault handler; RADV with the Vulkan 1.3 features and extensions the
backend asks for (`runtime/nvn/vulkan/device.c`); runtime shader compilation speed (shaderc, cached in `cache/`);
sound through the console's output; the films (FFmpeg); and memory: the guest heap is backed as the game sizes it,
beside RADV's GPU memory, out of the console's 12 GB of direct memory.

## Credits

- **LAS Recomp** (Peiikai): the recompilation and runtime this is a port of.
- **Mihawk ([mihawk-99](https://github.com/mihawk-99))**: Vulkan on the PS5 -
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), the [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) fork
  of RADV, the [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate) (the console layer in
  `ps5/src/console.c`, the link recipe) and the payload SDK fork's platform layer, on the
  [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) (John Törnblom).
- **[PS5_ReXGlueTemplate](https://github.com/KongaTime/PS5_ReXGlueTemplate)** (KongaTime): the title link and its
  checks, which `ps5/tools/link-title.sh` follows.
- **[etaHEN](https://github.com/etaHEN/etaHEN)** and **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**.

Made with Claude Code, like the rest of LAS Recomp.

## Licence

The port's own files are MIT, as LAS Recomp. The title as distributed links the platform layer and PS5_Vulkan,
which are GPL-3.0-or-later, so a built title is under GPL-3.0-or-later as a whole; `LEGAL.txt` and `licenses/`
beside each `eboot.bin` list every part with its source and revision. FFmpeg (LGPL-2.1-or-later) is linked
statically there, within that GPL-3.0-or-later whole, whose complete source is this repository and the scripts that
fetch every part at a pinned revision.

Not affiliated with or endorsed by Nintendo or Sony Interactive Entertainment. Use only games you own.
