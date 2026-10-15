// The game code library's loader on the PS5 (runtime/platform/ps5_elf.h).
//
// What the library is (ps5/tools/gamelib.py links it so): an x86-64 ELF shared object holding the recompiled
// code, tools/gamelib_entry.c and nothing else; no libraries of its own (no DT_NEEDED), no thread-local storage, no
// constructors; lasgame_tables as its entry point. Its relocations are the four a position-independent C library
// has: RELATIVE (the function tables), and 64 / GLOB_DAT / JUMP_SLOT against the names it imports, which are the
// runtime's (rt_*, the hooks the generator's patches call) and a few of libc's and the compiler's (memcpy,
// fesetround...). Each import is bound to the title's own function of that name (ps5_gamelib_imports); a library
// that needs one the title doesn't have is refused, with its name, rather than calling through NULL later.
//
// It is loaded into direct memory given execute (ps5platform/exec.h: read-write-execute, outside the GPU window),
// and stays for the life of the title (gamelib_load loads once).
#include "ps5_elf.h"

#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ps5platform/exec.h>

#ifndef R_X86_64_JUMP_SLOT
#define R_X86_64_JUMP_SLOT R_X86_64_JMP_SLOT  // (FreeBSD's name)
#endif
#ifndef DT_RELR
#define DT_RELR 36  // (newer than the SDK's FreeBSD headers)
#endif

static void *fail(char *why, size_t n, const char *path, const char *what) {
    snprintf(why, n, "%s: %s", path, what);
    return NULL;
}

static const Ps5GameImport *find_import(const char *name) {
    size_t lo = 0, hi = ps5_gamelib_import_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(ps5_gamelib_imports[mid].name, name);
        if (!c) return &ps5_gamelib_imports[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void *ps5_elf_load(const char *path, char *why, size_t why_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(why, why_size, path, "can't open it");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > (long)sizeof(Elf64_Ehdr) ? malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(file);
        return fail(why, why_size, path, "can't read it");
    }
    fclose(f);
    void *entry = NULL;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_type != ET_DYN ||
        eh->e_machine != EM_X86_64 || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof *ph > (uint64_t)size) {
        fail(why, why_size, path, "not an x86-64 shared object (build it with ps5/tools/gamelib.py)");
        goto out;
    }

    // the span of its segments, laid out in one region at their own offsets
    uint64_t span = 0;
    const Elf64_Phdr *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) { fail(why, why_size, path, "has thread-local storage"); goto out; }
        if (ph[i].p_type == PT_DYNAMIC) dyn = &ph[i];
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > (uint64_t)size || ph[i].p_filesz > ph[i].p_memsz) {
            fail(why, why_size, path, "a segment lies outside the file");
            goto out;
        }
        if (ph[i].p_vaddr + ph[i].p_memsz > span) span = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (!span || !dyn) { fail(why, why_size, path, "no segments to load"); goto out; }
    struct ps5_exec_request rq = {.bytes = (size_t)span};
    struct ps5_exec_region rg;
    int r = ps5_exec_alloc(&rq, &rg);
    if (r != 0) {
        char m[96];
        snprintf(m, sizeof m, "no executable memory for %llu MB (%#x)", (unsigned long long)(span >> 20), (unsigned)r);
        fail(why, why_size, path, m);
        goto out;
    }
    uint8_t *base = rg.base;  // (zeroed: the bss needs nothing more)
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) memcpy(base + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);

    // its dynamic section, as loaded
    const Elf64_Dyn *d = (const Elf64_Dyn *)(base + dyn->p_vaddr);
    const Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    uint64_t relasz = 0, pltrelsz = 0;
    for (; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = (const Elf64_Sym *)(base + d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)(base + d->d_un.d_ptr); break;
        case DT_RELA: rela = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_PLTREL:
            if (d->d_un.d_val != DT_RELA) { fail(why, why_size, path, "PLT relocations aren't RELA"); goto unmap; }
            break;
        case DT_NEEDED: fail(why, why_size, path, "needs other libraries"); goto unmap;
        case DT_INIT: case DT_INIT_ARRAY: case DT_PREINIT_ARRAY:
            fail(why, why_size, path, "has constructors");
            goto unmap;
        case DT_REL: case DT_RELR: fail(why, why_size, path, "has REL / RELR relocations"); goto unmap;
        default: break;
        }
    }
    if ((relasz || pltrelsz) && (!symtab || !strtab)) { fail(why, why_size, path, "no symbol table"); goto unmap; }

    // every relocation, both tables; the missing imports are listed together
    char missing[512] = "";
    size_t nmissing = 0;
    const Elf64_Rela *tabs[2] = {rela, jmprel};
    uint64_t counts[2] = {relasz / sizeof(Elf64_Rela), pltrelsz / sizeof(Elf64_Rela)};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; tabs[t] && i < counts[t]; i++) {
            const Elf64_Rela *rl = &tabs[t][i];
            uint32_t type = ELF64_R_TYPE(rl->r_info), si = ELF64_R_SYM(rl->r_info);
            uint64_t *at = (uint64_t *)(base + rl->r_offset);
            if (rl->r_offset + 8 > span) { fail(why, why_size, path, "a relocation lies outside it"); goto unmap; }
            if (type == R_X86_64_RELATIVE) { *at = (uint64_t)(uintptr_t)base + (uint64_t)rl->r_addend; continue; }
            if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
                char m[64];
                snprintf(m, sizeof m, "relocation type %u isn't supported", type);
                fail(why, why_size, path, m);
                goto unmap;
            }
            const Elf64_Sym *s = &symtab[si];
            uint64_t value;
            if (s->st_shndx != SHN_UNDEF) value = (uint64_t)(uintptr_t)base + s->st_value;  // (its own)
            else {
                const Ps5GameImport *im = find_import(strtab + s->st_name);
                if (!im && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
                    if (nmissing++ < 8 && !strstr(missing, strtab + s->st_name))
                        snprintf(missing + strlen(missing), sizeof missing - strlen(missing), "%s%s",
                                 missing[0] ? " " : "", strtab + s->st_name);
                    continue;
                }
                value = im ? (uint64_t)(uintptr_t)im->addr : 0;
            }
            *at = value + (type == R_X86_64_64 ? (uint64_t)rl->r_addend : 0);
        }
    if (nmissing) {
        char m[640];
        snprintf(m, sizeof m, "it needs what this build doesn't have (built for another version?): %s", missing);
        fail(why, why_size, path, m);
        goto unmap;
    }
    if (!eh->e_entry || eh->e_entry >= span) { fail(why, why_size, path, "no entry point"); goto unmap; }
    entry = base + eh->e_entry;
    fprintf(stderr, "[gamelib] %s: %llu MB at %p, %llu + %llu relocations\n", path,
            (unsigned long long)(span >> 20), (void *)base, (unsigned long long)counts[0], (unsigned long long)counts[1]);
    goto out;
unmap:
    ps5_exec_free(&rg);
out:
    free(file);
    return entry;
}
