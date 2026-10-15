#!/usr/bin/env python3
"""LAS Recomp on the PS5 - the game code, built on a PC from your own game files.

    ps5/tools/gamelib.py build EXTRACTED      the game code for the console: EXTRACTED/lasgame.ps5.so (+ .id)
    ps5/tools/gamelib.py id                   the id a title of this checkout loads (ps5/CMakeLists.txt asks)

EXTRACTED is a game folder LAS Recomp set up on a PC (the start screen's "Set up game files": exefs/, romfs/,
romfs.bin), or one laid out the same way. The console can neither run the generator nor compile, so this does both
here, as the start screen does on the desktop (runtime/gamebuild.c): tools/gen.py lifts exefs/main and exefs/sdk to C
(the portable FP/SIMD lowering, --simd c), and clang compiles it, with tools/gamelib_entry.c, into one x86-64 shared
object the title loads itself (runtime/platform/ps5_elf.c). Nothing of the console is needed for it: the code is
compiled against ps5/gamelib/include (the few C library names it calls) and clang's own headers, for any x86-64 ELF
target, and linked with no libraries; its imports are bound on the console by name.

Needs: Python 3.9+, clang 18 or later and lld (ld.lld) - on Linux, macOS (Homebrew llvm + lld) or WSL. The
generator's Python packages are installed into build/ps5-gamelib/venv the first time.

The library is tied to this checkout: its id (a hash of the generator, the headers the code is compiled against and
the flags, as the Makefile's GAMELIB_ID) must be the title's, or the title refuses it. Build both from the same commit.

SPDX-License-Identifier: MIT
"""
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(ROOT, 'build', 'ps5-gamelib')

# The Makefile's GENFLAGS for the console's CPU (Zen 2: AVX2, FMA, F16C, BMI2), and what makes the code depend on
# nothing but its imports: no stack protector or fortified calls (their helpers are libc's), no CET notes. Frame
# pointers kept: the console's crash report walks them (runtime/platform/ps5.c, plat_print_backtrace).
GENFLAGS = ('-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime '
            '-march=znver2 -fPIC -fno-omit-frame-pointer -fno-stack-protector -fcf-protection=none -fno-exceptions '
            '-fno-asynchronous-unwind-tables -U_FORTIFY_SOURCE')
TARGET = 'x86_64-unknown-linux-gnu'  # (any x86-64 ELF target: the System V ABI the console's code uses too)

# What the id covers: the Makefile's GAMELIB_SRCS
GAMELIB_SRCS = ['tools/gen.py', 'tools/lifter.py', 'tools/analyze.py', 'tools/nso.py', 'tools/passes.py',
                'tools/patches.py', 'tools/simd_c.py', 'tools/rando/patch_variants.json', 'tools/gamelib_entry.c',
                'runtime/hle/hle_symbols.txt', 'runtime/cpu.h', 'runtime/simd.h', 'runtime/simd_fp.h',
                'runtime/gamelib.h', 'ps5/gamelib/include/string.h', 'ps5/gamelib/include/math.h',
                'ps5/gamelib/include/fenv.h']
GEN_PACKAGES = ['capstone==5.0.9', 'unicorn==2.1.4', 'lz4==4.4.5']  # (the Makefile's, for tools/gen.py)
NAME = 'lasgame.ps5.so'


def gamelib_id():
    h = hashlib.sha256()
    for p in GAMELIB_SRCS:
        with open(os.path.join(ROOT, p), 'rb') as f:
            h.update(f.read())
    h.update((GENFLAGS + '\n').encode())
    return h.hexdigest()[:16]


def tool(env, names):
    for n in [os.environ.get(env)] + names:
        if n and shutil.which(n):
            return shutil.which(n)
    sys.exit(f'gamelib.py: none of {", ".join(names)} found (set {env})')


def venv_python():
    venv = os.path.join(WORK, 'venv')
    py = os.path.join(venv, 'Scripts' if os.name == 'nt' else 'bin', 'python')
    stamp = os.path.join(venv, '.ok')
    if not os.path.exists(stamp):
        subprocess.run([sys.executable, '-m', 'venv', venv], check=True)
        subprocess.run([py, '-m', 'pip', 'install', '-q'] + GEN_PACKAGES, check=True)
        open(stamp, 'w').close()
    return py


def compile_flags(cc, gid):
    """The compiler command for one unit of game code, without its -c / -o."""
    resource = subprocess.run([cc, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip()
    flags = [cc, f'--target={TARGET}', '-nostdinc', '-isystem', os.path.join(resource, 'include'),
             '-isystem', os.path.join(ROOT, 'ps5', 'gamelib', 'include'), f'-DGAMELIB_ID="{gid}"']
    return flags + [f.replace('-Iruntime', '-I' + os.path.join(ROOT, 'runtime')) for f in GENFLAGS.split()]


def link_command(lld, out, rsp):
    """One shared object with no libraries, lasgame_tables its entry, every relocation resolved at load (the loader
    binds all of them: no lazy binding), RELA only (no packed relocations): what runtime/platform/ps5_elf.c loads."""
    return [lld, '-shared', '-nostdlib', '-e', 'lasgame_tables', '-z', 'now', '-z', 'noexecstack',
            '--hash-style=sysv', '--build-id=none', '-o', out, '@' + rsp]


def wsl_path(p):
    """A Windows path given in WSL (C:\\Users\\... or C:/Users/...) as WSL names it: /mnt/c/Users/..."""
    m = re.match(r'^([A-Za-z]):[\\/](.*)$', p)
    if m and os.path.isdir('/mnt/' + m.group(1).lower()):
        return '/mnt/' + m.group(1).lower() + '/' + m.group(2).replace('\\', '/')
    return p


def build(game_dir):
    game_dir = os.path.abspath(wsl_path(game_dir))
    for nso in ('main', 'sdk'):
        if not os.path.isfile(os.path.join(game_dir, 'exefs', nso)):
            sys.exit(f'gamelib.py: no exefs/{nso} in {game_dir} (set the game files up with LAS Recomp on a PC first)')
    cc = tool('CC', ['clang', 'clang-20', 'clang-19', 'clang-18'])
    lld = tool('LLD', ['ld.lld', 'ld.lld-20', 'ld.lld-19', 'ld.lld-18'])
    py = venv_python()
    gid = gamelib_id()
    gen = os.path.join(WORK, 'generated')
    obj = os.path.join(WORK, 'obj')
    os.makedirs(obj, exist_ok=True)

    print(f'gamelib.py: generating C from {game_dir}/exefs (several minutes)', flush=True)
    hle = os.path.join(ROOT, 'runtime', 'hle', 'hle_symbols.txt')
    jobs = [[py, 'gen.py', 'main', os.path.join(game_dir, 'exefs', 'main'), '0x7000000000', os.path.join(gen, 'main'),
             '--simd', 'c'],
            [py, 'gen.py', 'sdk', os.path.join(game_dir, 'exefs', 'sdk'), '0x7004000000', os.path.join(gen, 'sdk'),
             '--hle', hle, '--simd', 'c']]
    procs = [subprocess.Popen(j, cwd=os.path.join(ROOT, 'tools'), stdout=subprocess.DEVNULL,
                              stderr=open(os.path.join(WORK, f'gen_{j[2]}.log'), 'w')) for j in jobs]
    for p, j in zip(procs, jobs):
        if p.wait():
            sys.exit(f'gamelib.py: the generator failed for {j[2]} ({os.path.join(WORK, "gen_" + j[2] + ".log")})')

    flags = compile_flags(cc, gid)
    units = [(os.path.join(ROOT, 'tools', 'gamelib_entry.c'), os.path.join(obj, 'gamelib_entry.o'))]
    for mod in ('main', 'sdk'):
        for fn in sorted(os.listdir(os.path.join(gen, mod))):
            if fn.endswith('.c'):
                units.append((os.path.join(gen, mod, fn), os.path.join(obj, f'{mod}_{fn[:-2]}.o')))

    def compile_one(u):
        src, o = u
        if os.path.exists(o) and os.path.getmtime(o) > os.path.getmtime(src):
            return None
        r = subprocess.run(flags + ['-c', src, '-o', o + '.tmp'], capture_output=True, text=True)
        if r.returncode:
            return f'{src}:\n{r.stderr[-2000:]}'
        os.replace(o + '.tmp', o)
        return None

    print(f'gamelib.py: compiling {len(units)} files', flush=True)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for err in ex.map(compile_one, units):
            done += 1
            if err:
                sys.exit(f'gamelib.py: compile failed: {err}')
            if done % 50 == 0:
                print(f'gamelib.py:   {done}/{len(units)}', flush=True)

    out = os.path.join(game_dir, NAME)
    rsp = os.path.join(WORK, 'link.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join(o for _, o in units))
    subprocess.run(link_command(lld, out + '.tmp', rsp), check=True)
    os.replace(out + '.tmp', out)
    with open(out + '.id', 'w') as f:
        f.write(gid + '\n')
    print(f'gamelib.py: {out} ({os.path.getsize(out) >> 20} MB, id {gid})')


def main():
    if len(sys.argv) == 2 and sys.argv[1] == 'id':
        print(gamelib_id())
    elif len(sys.argv) == 3 and sys.argv[1] == 'build':
        build(sys.argv[2])
    else:
        sys.exit(__doc__.split('\n\n')[1])


if __name__ == '__main__':
    main()
