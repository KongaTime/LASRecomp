#!/usr/bin/env python3
"""LAS Recomp on the PS5 - the title's home-screen art, kept out of git.

    ps5/tools/title-art.py

Your own art for the home screen: put the sources in ps5/title-art/, run this, and build (ps5/tools/build.sh).
ps5/title-art/ is ignored by git - art you didn't make (the game's key art, someone's fan art) stays on the
machine that builds the title - and ps5/tools/link-title.sh lays its sce_sys/ over the title's own in its folder.

- ps5/title-art/icon-source.*        -> sce_sys/icon0.png, 512x512 (scaled to cover, centred; pixel art - an image
                                       whose size divides 512 or 512 divides - is scaled without smoothing)
- ps5/title-art/background-source.*  -> sce_sys/pic0.dds and pic1.dds, the background the home screen shows
                                       behind the title, 3840x2160 (scaled to cover, centred), as the console takes
                                       it: DDS with a DX10 header, BC7_UNORM, one level, straight alpha

Needs Pillow and etcpak (pip install Pillow etcpak). Follows PS5_ReXGlueTemplate's tools/title-art.py.

The console keeps the name and art it read when the title was registered (ShadowMountPlus registers a
/data/homebrew title once), so new art in sce_sys/ doesn't show on the home screen by itself: upload a folder
named after the title ID holding only the new sce_sys/ with ps5upload, overriding the installed title, and it
registers again with the new art; the rest of its folder stays as it was.

SPDX-License-Identifier: MIT
"""
import struct
import sys
from pathlib import Path

import etcpak
from PIL import Image

ART = Path(__file__).resolve().parent.parent / "title-art"
SCE_SYS = ART / "sce_sys"
WIDTH, HEIGHT = 3840, 2160


def source(stem):
    found = sorted(p for p in ART.glob(stem + ".*") if p.is_file())
    if not found:
        sys.exit(f"title-art.py: no {ART / stem}.* (a .png, .jpg or .webp)")
    return found[0]


def cover(image, width, height, smooth=True):
    """Scaled to fill width x height, the overflow cut evenly from both sides."""
    scale = max(width / image.width, height / image.height)
    resized = image.resize((round(image.width * scale), round(image.height * scale)),
                           Image.LANCZOS if smooth else Image.NEAREST)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def dds_bc7(image):
    rgba = image.convert("RGBA")
    blocks = etcpak.compress_bc7(rgba.tobytes(), rgba.width, rgba.height)
    linear_size = (rgba.width // 4) * (rgba.height // 4) * 16
    assert len(blocks) == linear_size
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps, height, width, pixel format, mip count, linear size
    header = struct.pack("<4sIIIIIII11I", b"DDS ", 124, flags, rgba.height, rgba.width, linear_size, 1, 1, *([0] * 11))
    pixel_format = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 98, 3, 0, 1, 1)  # BC7_UNORM, 2D, no flags, one image, straight alpha
    return header + pixel_format + caps + dx10 + blocks


def main():
    SCE_SYS.mkdir(parents=True, exist_ok=True)
    src = Image.open(source("icon-source")).convert("RGB")
    pixel_art = 512 % src.width == 0 or src.width % 512 == 0
    cover(src, 512, 512, smooth=not pixel_art).save(SCE_SYS / "icon0.png", optimize=True)
    print(f"wrote {SCE_SYS / 'icon0.png'}")
    bg_src = Image.open(source("background-source")).convert("RGB")
    if max(WIDTH / bg_src.width, HEIGHT / bg_src.height) > 3:
        print(f"title-art.py: the background is {bg_src.width}x{bg_src.height}, scaled up "
              f"{max(WIDTH / bg_src.width, HEIGHT / bg_src.height):.1f}x to fill 3840x2160: a larger source looks sharper")
    data = dds_bc7(cover(bg_src, WIDTH, HEIGHT))
    for name in ("pic0.dds", "pic1.dds"):
        (SCE_SYS / name).write_bytes(data)
        print(f"wrote {SCE_SYS / name} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
