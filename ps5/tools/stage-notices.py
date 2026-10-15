#!/usr/bin/env python3
"""LAS Recomp on the PS5 - the notices a title folder carries: LEGAL.txt and licenses/.

    stage-notices.py APP PS5_VULKAN RADV_ARCHIVE SDK DEPS

Run by ps5/tools/link-title.sh. Every part linked into eboot.bin (or shipped beside it), its licence, its source and
the revision it was built from:

  LEGAL.txt              what the title is licensed under, and that no game is included
  licenses/README.txt    every part: licence, source, revision
  licenses/<part>/...    the licence texts

SPDX-License-Identifier: MIT
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def rev(repo):
    r = subprocess.run(['git', '-C', repo, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else 'unknown'


def main():
    app, vulkan, archive, sdk, deps = sys.argv[1:6]
    src = os.path.join(deps, 'src')
    out = os.path.join(app, 'licenses')
    os.makedirs(out, exist_ok=True)
    provenance = {}
    pfile = os.path.join(os.path.dirname(os.path.dirname(archive)), 'PROVENANCE.txt')
    if os.path.exists(pfile):
        for line in open(pfile):
            if ': ' in line:
                k, v = line.rstrip('\n').split(': ', 1)
                provenance[k] = v
    sdk_rev = open(os.path.join(sdk, '.ps5-sdk-revision')).read().strip()
    sdk_fork = os.environ.get('PS5_PAYLOAD_SDK_FORK', os.path.join(ROOT, '..', 'PS5_PayloadSDK'))
    mesa_src = os.path.join(vulkan, '.deps', 'work', 'radv-src')

    # (part, licence, source, revision, [(file, name in licenses/<id>/)])
    parts = [
        ('lasrecomp', 'LAS Recomp (the runtime, this port)', 'MIT', 'https://github.com/KongaTime/LASRecomp', rev(ROOT),
         [(os.path.join(ROOT, 'LICENSE'), 'LICENSE')]),
        ('ps5-console', 'PS5_VulkanTemplate\'s console layer (ps5/src/console.c)', 'MIT',
         'https://github.com/mihawk-99/PS5_VulkanTemplate', 'f827f0a',
         [(os.path.join(ROOT, 'ps5', 'src', 'console-LICENSE.txt'), 'LICENSE.txt')]),
        ('platform', 'PS5 payload SDK fork and its platform layer (libps5platform)', 'GPL-3.0-or-later',
         'https://github.com/KongaTime/PS5_PayloadSDK', sdk_rev, [('git:' + sdk_fork + ':' + sdk_rev + ':LICENSE', 'GPL-3.0.txt')]),
        ('ps5-vulkan', 'PS5_Vulkan (the RADV link, the CRT, libc.prx)', 'GPL-3.0-or-later',
         'https://github.com/mihawk-99/PS5_Vulkan', rev(vulkan), [(os.path.join(vulkan, 'LICENSE'), 'GPL-3.0.txt')]),
        ('radv', 'RADV, Mesa\'s Vulkan driver, with the PS5 winsys (PS5_Mesa)',
         'MIT, with other licences stated per file (Mesa-license.rst)', 'https://github.com/mihawk-99/PS5_Mesa',
         provenance.get('revision', 'unknown'),
         [(os.path.join(mesa_src, 'docs', 'license.rst'), 'Mesa-license.rst'), (os.path.join(mesa_src, 'licenses'), 'licenses')]),
        ('llvm-runtime', 'libc++, libc++abi, libunwind, compiler-rt builtins (from the payload SDK)',
         'Apache-2.0 WITH LLVM-exception', 'https://github.com/llvm/llvm-project', 'the payload SDK\'s',
         [(os.path.join(deps, 'prefix', 'llvm', 'LICENSE.TXT'), 'LICENSE.TXT')]),
        ('imgui', 'Dear ImGui', 'MIT', 'https://github.com/ocornut/imgui', 'third_party/imgui',
         [(os.path.join(ROOT, 'third_party', 'imgui', 'LICENSE.txt'), 'LICENSE.txt')]),
        ('smaa', 'SMAA', 'MIT', 'https://github.com/iryoku/smaa', 'third_party/smaa',
         [(os.path.join(ROOT, 'third_party', 'smaa', 'LICENSE.txt'), 'LICENSE.txt')]),
        ('baloo2', 'Baloo 2 font (Baloo2-SemiBold.ttf)', 'OFL-1.1', 'https://github.com/EkType/Baloo2', '1.640',
         [(os.path.join(ROOT, 'third_party', 'fonts', 'Baloo2', 'OFL.txt'), 'OFL.txt')]),
        ('sdl3', 'SDL 3 (its headers, and its scancode names in ps5/sdl/scancode_names.h)', 'Zlib',
         'https://github.com/libsdl-org/SDL', rev(os.path.join(src, 'sdl')), [(os.path.join(src, 'sdl', 'LICENSE.txt'), 'LICENSE.txt')]),
        ('vulkan-headers', 'Vulkan-Headers', 'Apache-2.0 OR MIT', 'https://github.com/KhronosGroup/Vulkan-Headers',
         rev(os.path.join(src, 'vulkan-headers')), [(os.path.join(src, 'vulkan-headers', 'LICENSE.md'), 'LICENSE.md')]),
        ('volk', 'volk', 'MIT', 'https://github.com/zeux/volk', rev(os.path.join(src, 'volk')),
         [(os.path.join(src, 'volk', 'LICENSE.md'), 'LICENSE.md')]),
        ('vma', 'Vulkan Memory Allocator', 'MIT', 'https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator',
         rev(os.path.join(src, 'vma')), [(os.path.join(src, 'vma', 'LICENSE.txt'), 'LICENSE.txt')]),
        ('shaderc', 'shaderc, glslang, SPIRV-Tools, SPIRV-Headers', 'Apache-2.0 (glslang: BSD-3-Clause and others, its LICENSE.txt)',
         'https://github.com/google/shaderc', rev(os.path.join(src, 'shaderc')),
         [(os.path.join(src, 'shaderc', 'LICENSE'), 'shaderc-LICENSE'),
          (os.path.join(src, 'shaderc', 'third_party', 'glslang', 'LICENSE.txt'), 'glslang-LICENSE.txt'),
          (os.path.join(src, 'shaderc', 'third_party', 'spirv-tools', 'LICENSE'), 'spirv-tools-LICENSE'),
          (os.path.join(src, 'shaderc', 'third_party', 'spirv-headers', 'LICENSE'), 'spirv-headers-LICENSE')]),
        ('ffmpeg', 'FFmpeg (libavformat, libavcodec, libswscale, libavutil: the LGPL configuration, linked statically)',
         'LGPL-2.1-or-later', 'https://github.com/FFmpeg/FFmpeg', rev(os.path.join(src, 'ffmpeg')),
         [(os.path.join(src, 'ffmpeg', 'COPYING.LGPLv2.1'), 'COPYING.LGPLv2.1'),
          (os.path.join(src, 'ffmpeg', 'LICENSE.md'), 'LICENSE.md')]),
    ]
    readme = [open(os.path.join(ROOT, 'ps5', 'LEGAL.txt')).read(), '', 'The parts of this title:', '']
    for pid, name, lic, url, revision, texts in parts:
        d = os.path.join(out, pid)
        os.makedirs(d, exist_ok=True)
        for path, dest in texts:
            if path.startswith('git:'):
                _, repo, r, p = path.split(':', 3)
                g = subprocess.run(['git', '-C', repo, 'show', f'{r}:{p}'], capture_output=True)
                if g.returncode == 0:
                    open(os.path.join(d, dest), 'wb').write(g.stdout)
            elif os.path.isdir(path):
                shutil.copytree(path, os.path.join(d, dest), dirs_exist_ok=True)
            elif os.path.exists(path):
                shutil.copy2(path, os.path.join(d, dest))
        readme += [name, f'  licence:  {lic}', f'  source:   {url}', f'  revision: {revision}',
                   f'  texts:    licenses/{pid}/', '']
    open(os.path.join(out, 'README.txt'), 'w').write('\n'.join(readme))
    shutil.copy2(os.path.join(ROOT, 'ps5', 'LEGAL.txt'), os.path.join(app, 'LEGAL.txt'))
    shutil.copy2(os.path.join(ROOT, 'README.md'), os.path.join(app, 'README.md'))


if __name__ == '__main__':
    main()
