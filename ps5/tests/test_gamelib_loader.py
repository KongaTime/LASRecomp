#!/usr/bin/env python3
"""LAS Recomp on the PS5 - the game code library's path from the PC's build to the console's loader, on the PC.

    python3 ps5/tests/test_gamelib_loader.py

Builds a stand-in for the game code (what tools/gen.py writes: chunks of f_<addr> functions, the tables, and
tools/gamelib_entry.c), compiled and linked exactly as ps5/tools/gamelib.py does it (its compile_flags and
link_command: against runtime/cpu.h, runtime/simd.h and ps5/gamelib/include alone), then loads it with
runtime/platform/ps5_elf.c on this machine - the console's executable memory (ps5platform/exec.h) stood in for by
mmap - and calls it: the function tables, every kind of relocation, imports of the runtime's functions and data,
libc's and compiler-rt's, and a library needing a name the title lacks, which must be refused with that name.

Needs clang and ld.lld (as gamelib.py), on x86-64 Linux.

SPDX-License-Identifier: MIT
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'ps5', 'tools'))
import gamelib  # noqa: E402

# Generated-code stand-ins, in tools/gen.py's form
MAIN = r'''
#include "cpu.h"
#include "simd.h"
#include <math.h>
extern uint32_t rando_word(uint64_t);
void f_7000000100(Cpu *c);
void f_7000000000(Cpu *c) {
    FN_ENTER(0x7000000000ull);
    c->x[0] = rt_ticks() + rando_word(0x7000000000ull);  /* imports: runtime functions */
    uint8_t buf[24] = {1, 2, 3};
    memcpy((void *)(uintptr_t)c->x[1], buf, sizeof buf);    /* libc */
    c->x[2] = (uint64_t)(int64_t)round((double)(int64_t)c->x[3] / 2.0);  /* libm */
    volatile __uint128_t n = ((__uint128_t)c->x[4] << 64) | c->x[5], d = c->x[6];
    c->x[7] = (uint64_t)(n / d);                            /* compiler-rt: __udivti3 */
    int old = fegetround();
    fesetround(FE_UPWARD);
    c->x[8] = (uint64_t)fegetround();
    fesetround(old);
    c->x[30] = 0x7000000004ull;
    f_7000000100(c);                                         /* its own function, through the PLT or directly */
    c->pc = 0x7000000200ull;
    __attribute__((musttail)) return rt_dispatch(c);         /* the runtime again, a tail call */
}
void f_7000000100(Cpu *c) { c->x[9] = 0x100; }
'''
MAIN_TABLE = r'''
#include "cpu.h"
void f_7000000000(Cpu *c);
void f_7000000100(Cpu *c);
const uint64_t main_fn_addrs[] = {0x7000000000ull, 0x7000000100ull};
const GuestFn main_fn_ptrs[] = {f_7000000000, f_7000000100};
const uint64_t main_fn_count = 2;
'''
SDK_TABLE = r'''
#include "cpu.h"
void f_7004000000(Cpu *c) { c->x[0] = 0x5d4; }
const uint64_t sdk_fn_addrs[] = {0x7004000000ull};
const GuestFn sdk_fn_ptrs[] = {f_7004000000};
const uint64_t sdk_fn_count = 1;
'''
MISSING = r'''
#include "cpu.h"
extern void rt_not_in_this_title(Cpu *);
void f_7004000000(Cpu *c) { rt_not_in_this_title(c); }
const uint64_t sdk_fn_addrs[] = {0x7004000000ull};
const GuestFn sdk_fn_ptrs[] = {f_7004000000};
const uint64_t sdk_fn_count = 1;
'''

# The console's executable memory, on this machine
EXEC_H = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
struct ps5_exec_request { size_t bytes; uintptr_t address, anchor; unsigned flags; };
struct ps5_exec_region { void *base, *write_view; size_t bytes; int64_t direct_start; unsigned flags; };
static inline int ps5_exec_alloc(const struct ps5_exec_request *rq, struct ps5_exec_region *rg) {
    size_t n = (rq->bytes + 0xffff) & ~(size_t)0xffff;
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    *rg = (struct ps5_exec_region){p, p, n, 0, 0};
    return 0;
}
static inline void ps5_exec_free(struct ps5_exec_region *rg) { munmap(rg->base, rg->bytes); }
'''

# The title's side: its runtime functions (here recording what the game code did), libc's and compiler-rt's
HARNESS = r'''
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "gamelib.h"
#include "platform/ps5_elf.h"

volatile int g_fnprof = 0;
static int fnprof_calls, dispatched;
static uint64_t dispatched_pc;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) { (void)a, (void)lr, (void)x0; fnprof_calls++; }
uint64_t rt_ticks(void) { return 40; }
uint32_t rando_word(uint64_t a) { return a == 0x7000000000ull ? 2 : 0; }
void rt_dispatch(Cpu *c) { dispatched++; dispatched_pc = c->pc; }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c, (void)imm, (void)pc; }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c, (void)imm, (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c, (void)pc; abort(); }
extern char __udivti3;

const Ps5GameImport ps5_gamelib_imports[] = {  /* (sorted, as imports.py writes it) */
    {"__udivti3", &__udivti3},
    {"fegetround", (void *)fegetround},
    {"fesetround", (void *)fesetround},
    {"fnprof_hit", (void *)fnprof_hit},
    {"g_fnprof", (void *)&g_fnprof},
    {"memcpy", (void *)memcpy},
    {"rando_word", (void *)rando_word},
    {"round", (void *)round},
    {"rt_brk", (void *)rt_brk},
    {"rt_dispatch", (void *)rt_dispatch},
    {"rt_svc", (void *)rt_svc},
    {"rt_ticks", (void *)rt_ticks},
    {"rt_udf", (void *)rt_udf},
};
const size_t ps5_gamelib_import_count = sizeof ps5_gamelib_imports / sizeof *ps5_gamelib_imports;

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv) {
    char why[1024] = "";
    GameLibEntry entry = (GameLibEntry)ps5_elf_load(argv[1], why, sizeof why);
    if (!entry) { printf("FAIL load: %s\n", why); return 1; }
    const GameLibTables *t = entry();
    CHECK(t && t->abi == GAMELIB_ABI && !strcmp(t->id, GAMELIB_ID));
    CHECK(*t->main_count == 2 && *t->sdk_count == 1);
    CHECK(t->main_addrs[0] == 0x7000000000ull && t->main_addrs[1] == 0x7000000100ull);
    CHECK(t->sdk_addrs[0] == 0x7004000000ull);
    static Cpu c;
    uint8_t buf[24] = {0};
    c.x[1] = (uint64_t)(uintptr_t)buf;
    c.x[3] = 7;                         /* round(3.5) = 4 */
    c.x[4] = 1, c.x[5] = 0, c.x[6] = 4; /* 2^64 / 4 = 2^62 */
    g_fnprof = 1;
    t->main_fns[0](&c);
    CHECK(fnprof_calls == 1);
    CHECK(c.x[0] == 42);
    CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 3);
    CHECK(c.x[2] == 4);
    CHECK(c.x[7] == (1ull << 62));
    CHECK(c.x[8] == FE_UPWARD && fegetround() == FE_TONEAREST);
    CHECK(c.x[9] == 0x100);
    CHECK(dispatched == 1 && dispatched_pc == 0x7000000200ull);
    t->sdk_fns[0](&c);
    CHECK(c.x[0] == 0x5d4);

    /* a library needing what the title lacks: refused, naming it */
    why[0] = 0;
    CHECK(ps5_elf_load(argv[2], why, sizeof why) == NULL);
    CHECK(strstr(why, "rt_not_in_this_title") != NULL);
    printf("refused as it should be: %s\n", why);
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("PASS: the game code library loads, binds and runs\n");
    return 0;
}
'''


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'test: {" ".join(cmd[:3])}... failed:\n{r.stdout}{r.stderr}')
    return r.stdout


def main():
    cc = gamelib.tool('CC', ['clang', 'clang-20', 'clang-19', 'clang-18'])
    lld = gamelib.tool('LLD', ['ld.lld', 'ld.lld-20', 'ld.lld-19', 'ld.lld-18'])
    gid = gamelib.gamelib_id()
    flags = gamelib.compile_flags(cc, gid)
    with tempfile.TemporaryDirectory() as w:
        def lib(name, sources):
            objs = []
            for i, text in enumerate(sources):
                src = os.path.join(w, f'{name}_{i}.c')
                open(src, 'w').write(text)
                objs.append(src[:-2] + '.o')
                run(flags + ['-c', src, '-o', objs[-1]])
            objs.append(os.path.join(w, f'{name}_entry.o'))
            run(flags + ['-c', os.path.join(ROOT, 'tools', 'gamelib_entry.c'), '-o', objs[-1]])
            rsp = os.path.join(w, f'{name}.rsp')
            open(rsp, 'w').write('\n'.join(objs))
            out = os.path.join(w, f'{name}.so')
            run(gamelib.link_command(lld, out, rsp))
            return out
        good = lib('good', [MAIN, MAIN_TABLE, SDK_TABLE])
        bad = lib('bad', [MAIN, MAIN_TABLE, MISSING])
        print(run(['llvm-readelf-18' if not os.path.exists('/usr/bin/llvm-readelf') else 'llvm-readelf', '-r', good])
              .count('R_X86_64'), 'relocations in the stand-in library')
        os.makedirs(os.path.join(w, 'inc', 'ps5platform'))
        open(os.path.join(w, 'inc', 'ps5platform', 'exec.h'), 'w').write(EXEC_H)
        harness = os.path.join(w, 'harness.c')
        open(harness, 'w').write(HARNESS)
        exe = os.path.join(w, 'harness')
        run([cc, '-O1', '-g', '-I', os.path.join(w, 'inc'), '-I', os.path.join(ROOT, 'runtime'),
             f'-DGAMELIB_ID="{gid}"', harness, os.path.join(ROOT, 'runtime', 'platform', 'ps5_elf.c'), '-o', exe,
             '-lm', '-rtlib=compiler-rt', '-fuse-ld=lld'])
        r = subprocess.run([exe, good, bad], capture_output=True, text=True)
        sys.stdout.write(r.stdout + r.stderr)
        sys.exit(r.returncode)


if __name__ == '__main__':
    main()
