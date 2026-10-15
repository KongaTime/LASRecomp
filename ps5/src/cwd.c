// The working directory on the PS5. The runtime makes its data root (/app0) the working directory and opens
// everything by relative name (runtime/paths.c: extracted/, save/, cache/, larecomp.ini); the libraries it hands
// those names to (FFmpeg's file reads) do the same. A title's sandbox may refuse chdir; then the title keeps its
// working directory itself: chdir to an existing folder succeeds and is recorded, getcwd answers it, and every path
// call the title makes resolves a relative name against it. The link wraps them (ps5/tools/link-title.sh:
// --wrap=<name> for each below), so this holds for the runtime and every library alike. Where the console's own
// chdir works, it is used and nothing is rewritten.
//
// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

static char g_cwd[1024];  // the recorded working directory, "" while the console's own is used

// path made absolute against the recorded working directory (into buf), or path itself
static const char *fix(const char *path, char *buf, size_t n) {
    if (!g_cwd[0] || !path || path[0] == '/' || !path[0]) return path;
    if (path[0] == '.' && path[1] == '/') path += 2;
    if (path[0] == '.' && !path[1]) return g_cwd;
    snprintf(buf, n, "%s/%s", g_cwd, path);
    return buf;
}
#define FIX(p) fix(p, (char[1100]){0}, 1100)

int __real_chdir(const char *path);
char *__real_getcwd(char *buf, size_t size);

int __wrap_chdir(const char *path) {
    const char *p = FIX(path);
    if (__real_chdir(p) == 0) { g_cwd[0] = 0; return 0; }
    int e = errno;
    struct stat st;
    if (p[0] != '/' || stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = e ? e : ENOTDIR; return -1; }
    static int said;
    if (!said++) fprintf(stderr, "[title] chdir(%s) refused (errno %d): the title keeps its working directory\n", p, e);
    snprintf(g_cwd, sizeof g_cwd, "%s", p);
    size_t n = strlen(g_cwd);
    while (n > 1 && g_cwd[n - 1] == '/') g_cwd[--n] = 0;
    return 0;
}

char *__wrap_getcwd(char *buf, size_t size) {
    if (!g_cwd[0]) return __real_getcwd(buf, size);
    size_t n = strlen(g_cwd) + 1;
    if (!buf) return strdup(g_cwd);
    if (size < n) { errno = ERANGE; return NULL; }
    memcpy(buf, g_cwd, n);
    return buf;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return __real_open(FIX(path), flags, mode);
}

// one path argument, the rest passed on
#define WRAP1(ret, name, params, args)                    \
    ret __real_##name params;                             \
    ret __wrap_##name params { return __real_##name args; }
WRAP1(FILE *, fopen, (const char *path, const char *mode), (FIX(path), mode))
WRAP1(int, stat, (const char *path, struct stat *st), (FIX(path), st))
WRAP1(int, lstat, (const char *path, struct stat *st), (FIX(path), st))
WRAP1(int, mkdir, (const char *path, mode_t mode), (FIX(path), mode))
WRAP1(int, rmdir, (const char *path), (FIX(path)))
WRAP1(int, unlink, (const char *path), (FIX(path)))
WRAP1(int, remove, (const char *path), (FIX(path)))
WRAP1(int, chmod, (const char *path, mode_t mode), (FIX(path), mode))
WRAP1(int, truncate, (const char *path, off_t length), (FIX(path), length))
WRAP1(int, utime, (const char *path, const struct utimbuf *times), (FIX(path), times))
WRAP1(int, utimes, (const char *path, const struct timeval times[2]), (FIX(path), times))
WRAP1(int, access, (const char *path, int mode), (FIX(path), mode))
WRAP1(DIR *, opendir, (const char *path), (FIX(path)))
WRAP1(char *, realpath, (const char *path, char *resolved), (FIX(path), resolved))

int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to) { return __real_rename(FIX(from), FIX(to)); }
