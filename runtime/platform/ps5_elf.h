// The game code library on the PS5 (runtime/gamelib.c): a title cannot load a library it brought (dlopen and
// sceKernelLoadStartModule refuse it), so the library the PC built (ps5/tools/gamelib.py) is loaded by
// runtime/platform/ps5_elf.c: an ELF shared object of the recompiled code alone, its imports bound to the title's
// own functions by name (ps5_gamelib_imports, made by the title's build from the runtime: ps5/tools/imports.py).
#pragma once
#include <stddef.h>

typedef struct { const char *name; void *addr; } Ps5GameImport;
extern const Ps5GameImport ps5_gamelib_imports[];  // sorted by name
extern const size_t ps5_gamelib_import_count;

// Loads the library at path into executable direct memory and returns its entry point (the ELF's e_entry: the
// library is linked with lasgame_tables as its entry), or NULL with why filled in.
void *ps5_elf_load(const char *path, char *why, size_t why_size);
