// PS5 host layer (runtime/platform.h): a homebrew title on a jailbroken console, built with the PS5 payload SDK fork and
// its platform layer (ps5platform/*.h; ps5/README.md). The console is FreeBSD-like underneath, without child
// processes, dynamic loading or a window system.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <ps5platform/context.h>
#include <ps5platform/fp.h>
#include <ps5platform/klog.h>
#include <ps5platform/libc.h>
#include <ps5platform/shm.h>

#include "../platform.h"

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelUsleep(uint32_t microseconds);

// ---- guest address space
// As on Windows: the window is reserved up front (ps5_vrange_reserve_at, 12.5 GB at 0x70_0000_0000, where the console
// grants 16 GB at the hint itself) and backed per region by direct memory (ps5_vrange_commit: 64K units, a title's
// own mappings would be charged to its small flexible budget). plat_commit backs what the loader and the kernel map;
// anything else the guest touches is backed at its first access by the fault handler below. The OS does not fault
// for us (a read() into an unbacked guest buffer fails with EFAULT): such buffers are inside committed regions.
//
// A bit per 16K page says it is backed, so a page made inaccessible on purpose (plat_protect: the watchpoint,
// rando.c) faults as asked instead of being backed again.
static uint64_t g_lo, g_hi;
static _Atomic uint64_t *g_backed;
#define UNIT 0x10000ull  // the direct-memory unit ps5_vrange_commit backs

static bool page_backed(uint64_t a) {
    uint64_t p = (a - g_lo) / PLAT_PAGE;
    return (atomic_load_explicit(&g_backed[p / 64], memory_order_relaxed) >> (p % 64)) & 1;
}

static void mark_backed(uint64_t a, uint64_t e) {
    for (uint64_t p = (a - g_lo) / PLAT_PAGE, pe = (e - g_lo + PLAT_PAGE - 1) / PLAT_PAGE; p < pe; p++)
        atomic_fetch_or_explicit(&g_backed[p / 64], 1ull << (p % 64), memory_order_relaxed);
}

static bool lazy_commit(uint64_t a) {
    if (!g_backed || a < g_lo || a >= g_hi || page_backed(a)) return false;
    uint64_t u = a & ~(UNIT - 1);
    // (pages of this unit someone made inaccessible stay that way: only the unbacked ones are opened)
    for (uint64_t p = u; p < u + UNIT; p += PLAT_PAGE) {
        if (page_backed(p)) continue;
        if (ps5_vrange_commit((void *)(uintptr_t)p, PLAT_PAGE, PS5_SHM_READ | PS5_SHM_WRITE) != 0) return false;
        mark_backed(p, p + PLAT_PAGE);
    }
    return true;
}

// ---- guest threads' host stacks
// Guest recursion runs on the host stack, so kernel.c asks for 256 MB a thread. libkernel takes a stack it is asked
// for from flexible memory (about 400 MB for the whole title: one such stack would nearly use it up), so each guest
// thread's stack is a range of its own, outside the GPU window, backed at its top and below that a unit at a time as
// the thread reaches it (the fault handler, on the thread's alternate signal stack).
#define STACK_HINT 0x4000000000ull  // (16 GB granted at the hint from 0x10_0000_0000 up: docs/PROBE.md, "Virtual space")
#define STACK_TOP_BACKED 0x100000ull
#define MAX_STACKS 512
static struct { _Atomic uint64_t lo; uint64_t hi; } g_stacks[MAX_STACKS];

static bool stack_commit(uint64_t a) {
    for (int i = 0; i < MAX_STACKS; i++) {
        uint64_t lo = atomic_load_explicit(&g_stacks[i].lo, memory_order_acquire);
        if (!lo || a < lo || a >= g_stacks[i].hi) continue;
        uint64_t u = a & ~(UNIT - 1);
        return ps5_vrange_commit((void *)(uintptr_t)u, UNIT, PS5_SHM_READ | PS5_SHM_WRITE) == 0;
    }
    return false;
}

void *plat_thread_stack(size_t size) {
    size = (size + UNIT - 1) & ~(UNIT - 1);
    void *base = NULL;
    if (ps5_vrange_reserve(size, (void *)(uintptr_t)STACK_HINT, UNIT, &base) != 0) return NULL;
    uint64_t lo = (uint64_t)(uintptr_t)base, hi = lo + size;
    if (ps5_vrange_commit((void *)(uintptr_t)(hi - STACK_TOP_BACKED), STACK_TOP_BACKED, PS5_SHM_READ | PS5_SHM_WRITE)) {
        ps5_vrange_release(base, size);
        return NULL;
    }
    for (int i = 0; i < MAX_STACKS; i++) {
        uint64_t none = 0;
        if (atomic_load(&g_stacks[i].lo)) continue;
        g_stacks[i].hi = hi;
        if (atomic_compare_exchange_strong(&g_stacks[i].lo, &none, lo)) return base;
    }
    ps5_vrange_release(base, size);  // (more threads than slots: the caller lets pthread place one)
    return NULL;
}

void plat_thread_stack_free(void *stack) {
    for (int i = 0; i < MAX_STACKS; i++)
        if (atomic_load(&g_stacks[i].lo) == (uint64_t)(uintptr_t)stack) {
            ps5_vrange_release(stack, g_stacks[i].hi - (uint64_t)(uintptr_t)stack);
            atomic_store(&g_stacks[i].lo, 0);
            return;
        }
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    if (ps5_vrange_reserve_at((void *)(uintptr_t)lo, hi - lo) != 0) return false;
    uint64_t words = ((hi - lo) / PLAT_PAGE + 63) / 64;
    g_backed = calloc(words, sizeof *g_backed);
    if (!g_backed) { ps5_vrange_release((void *)(uintptr_t)lo, hi - lo); return false; }
    g_lo = lo;
    g_hi = hi;
    return true;
}

// Every unit given back (zero at its next commit), the range still reserved.
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    if (ps5_vrange_decommit((void *)(uintptr_t)lo, hi - lo) != 0) return false;
    memset((void *)g_backed, 0, ((g_hi - g_lo) / PLAT_PAGE + 63) / 64 * sizeof *g_backed);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size || addr < g_lo || addr + size > g_hi) return;
    uint64_t a = addr & ~(PLAT_PAGE - 1), e = (addr + size + PLAT_PAGE - 1) & ~(PLAT_PAGE - 1);
    // page by page over what is backed already: a page made inaccessible on purpose keeps its protection
    for (uint64_t p = a; p < e;) {
        if (page_backed(p)) { p += PLAT_PAGE; continue; }
        uint64_t q = p;
        while (q < e && !page_backed(q)) q += PLAT_PAGE;
        int r = ps5_vrange_commit((void *)(uintptr_t)p, q - p, PS5_SHM_READ | PS5_SHM_WRITE);
        if (r != 0) {
            fprintf(stderr, "[plat] commit %llx+%llx failed (%#x)\n", (unsigned long long)p,
                    (unsigned long long)(q - p), (unsigned)r);
            return;
        }
        mark_backed(p, q);
        p = q;
    }
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr, (void)size, (void)pages, (void)max, (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? PS5_SHM_READ : PS5_SHM_READ | PS5_SHM_WRITE;
    if (addr >= g_lo && addr + size <= g_hi) {
        // (backs what isn't yet, as Windows' VirtualProtect after its commit)
        if (ps5_vrange_commit((void *)(uintptr_t)addr, size, p) != 0) return -1;
        mark_backed(addr & ~(PLAT_PAGE - 1), addr + size);
        return 0;
    }
    return mprotect((void *)(uintptr_t)addr, size, prot == PLAT_PROT_NONE ? PROT_NONE
                                                  : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

// A relative sleep to just short of t, then a spin on the clock: the console's timer slack is unmeasured, and the
// vsync thread (plat_thread_realtime) needs its wakeups on time.
void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 300000) { sched_yield(); continue; }
        uint64_t s = left - 200000;
        struct timespec ts = {.tv_sec = (time_t)(s / 1000000000u), .tv_nsec = (long)(s % 1000000000u)};
        nanosleep(&ts, NULL);
    }
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = sched_get_priority_max(SCHED_FIFO)};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);  // (may be refused: then as it was)
}

// ---- process
// The log: everything written to stderr goes straight to lasrecomp.log in the title folder (fetched over FTP: the
// record a run leaves), line-buffered and written by the thread that writes it, so a crash report is in the file
// before the title goes (a pipe drained by a thread of its own lost the last lines, the report among them).
static char g_log_path[256];
static int g_log_fd = -1;

static void log_start(void) {
    snprintf(g_log_path, sizeof g_log_path, "/app0/lasrecomp.log");
    // (a log of this run only: the one before is kept for a crash that ended it)
    rename("/app0/lasrecomp.log", "/app0/lasrecomp.previous.log");
    g_log_fd = open(g_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_log_fd < 0) return;
    fchmod(g_log_fd, 0666);  // (FTP is another process: whatever the umask gave)
    chmod("/app0/lasrecomp.previous.log", 0666);
    FILE *f = fdopen(g_log_fd, "w");
    if (!f) return;
    setvbuf(f, NULL, _IOLBF, 0);
    fflush(stderr);
    stderr = f;
    stdout = f;
}

int sceSystemServiceHideSplashScreen(void);

void plat_init(void) {
    // A title starts flush-to-zero / denormals-are-zero (MXCSR 0x9fe0): the guest's FP (the portable lowering in
    // simd.h) and the host's own maths expect IEEE, as on every other system. Set before any thread is made: threads
    // start with their creator's MXCSR (the platform layer's pthread_create wrap).
    ps5_fp_ieee();
    // What the title makes (saves, settings, caches, the log) is fetched and copied over FTP, another process: no
    // permission bits taken away from what the runtime asks for (the console's own umask is refused: ps5platform/libc.h)
    ps5_umask(0);
    log_start();
    fprintf(stderr, "[plat] PS5: log %s\n", g_log_path);
    // How the title's folder answers (the data root: runtime/paths.c needs it a writable folder it can make current)
    struct stat st;
    int sr = stat("/app0", &st), se = sr ? errno : 0;
    FILE *t = fopen("/app0/.write_test", "wb");
    int fe = t ? 0 : errno;
    if (t) fclose(t), remove("/app0/.write_test");
    fprintf(stderr, "[plat] /app0: stat %d (errno %d, folder %d), write %s (errno %d)\n", sr, se,
            sr == 0 && S_ISDIR(st.st_mode), t ? "ok" : "refused", fe);
    sceSystemServiceHideSplashScreen();  // (the shell's splash covers every frame until then)
}

// The title's own ELF, as the console names it (no /proc): for rt_resource's "next to the executable", the title
// folder.
bool plat_exe_path(char *buf, size_t size) {
    return (size_t)snprintf(buf, size, "/app0/eboot.bin") < size;
}

// No processes of our own: the in-place reboot (main.c) replaces re-executing; nothing on the console calls this.
void plat_exec(char *const argv[]) { (void)argv; }

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    (void)pc, (void)offset;
    return NULL;  // (the title's ELF keeps no symbol table: rt_host_fn_name names guest functions)
}

void plat_fatal_notice(void) { fflush(stderr); }

// Frame pointers (the title is built with them): each frame's return address. The pcs are matched against the
// linked ELF (build/ps5/link/) afterwards; guest functions are named by the caller (rt_host_fn_name).
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
    for (int i = 0; i < 48 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        uint64_t ret = fp[1], off = 0;
        if (!ret) break;
        const char *s = rt_host_fn_name(ret, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)ret, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx\n", i, (unsigned long long)ret);
        uint64_t *next = (uint64_t *)fp[0];
        if (next <= fp) break;  // (stacks grow down: a frame further up is at a higher address)
        fp = next;
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    static __thread int reporting;  // (a fault while reporting one: the console's own report, not a loop)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if (reporting) { signal(sig, SIG_DFL); return; }
    // first touch of an unbacked guest page, or of a guest thread's stack further down
    if ((sig == SIGSEGV || sig == SIGBUS) && (lazy_commit(fa) || stack_commit(fa))) return;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    // the console's context, with the SDK fork's layout (ps5platform/context.h)
    uint64_t pc = (uint64_t)uc->uc_mcontext.mc_rip, sp = (uint64_t)uc->uc_mcontext.mc_rsp, lr = 0;
    if (rt_fault_watch(fa, pc)) return;
    reporting = 1;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

static void install_handlers(void);

void plat_crash_handler_install(void) {
    static __thread bool done;  // (per thread: its alternate stack)
    if (done) return;
    done = true;
    stack_t ss = {.ss_sp = malloc(1 << 18), .ss_size = 1 << 18};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, install_handlers);
}

static void install_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes: none on the console (game files, seeds and the game code are set up on a PC: ps5/README.md)
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)log_path, (void)append_log;
    fprintf(stderr, "[plat] can't start %s on the PS5\n", argv && argv[0] ? argv[0] : "?");
    return 0;
}
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 255; }
void plat_proc_kill(PlatProc p) { (void)p; }

static bool remove_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return remove(p) == 0;
    DIR *d = opendir(p);
    if (!d) return false;
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        ok &= remove_tree(c);
    }
    closedir(d);
    return rmdir(p) == 0 && ok;
}
bool plat_remove_tree(const char *dir) { return remove_tree(dir); }

// The only way a title ends: the shell closes it (exit() is reported as a crash by the console). It does so
// asynchronously: wait for it.
void plat_terminate(void) {
    fflush(NULL);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}
