/*  node-ios shim -- lets the official macOS arm64 Node.js binary run on iOS.
 *
 *  Node reaches these through patched LC_DYLD_CHAINED_FIXUPS import ordinals
 *  (nodeios_patch.py), not DYLD_INSERT_LIBRARIES: dyld ignores DYLD_* for an
 *  entitled binary, and the two-level import table would never pick an
 *  inserted library anyway.
 *
 *  Three of Node's 725 imports are absent from iOS's libraries; the rest of
 *  what is interposed here is for JIT and for running programs:
 *
 *  1. pthread_jit_write_protect_np -- macOS-only (SDK marks it
 *     __API_UNAVAILABLE(ios); absent on iOS 17.3). V8's arm64 Darwin build maps
 *     its code ranges MAP_JIT and flips W^X per thread through this call. iOS
 *     rejects MAP_JIT and SIGBUSes on executing a writable page, but mprotect()
 *     may move a page between RW and RX, so the shim repairs faults one page at
 *     a time. mmap, mprotect, munmap, sigaction and signal are interposed for
 *     it. See section 1.
 *
 *  2. SecTrustSettingsCopyTrustSettings -- macOS-only Security API behind
 *     Node's --use-system-ca. Report "no trust settings" so Node falls back to
 *     its bundled Mozilla root store, which is what we want on iOS anyway.
 *
 *  3. syslog$DARWIN_EXTSN -- iOS exports plain syslog but not the
 *     $DARWIN_EXTSN alias. Forward to it.
 *
 *  4. exec path remapping. A rootless jailbreak has no /bin/sh -- the bootstrap
 *     lives under /var/jb. Node hard-codes /bin/sh for `shell: true`, so
 *     execSync and every npm lifecycle script fail with ENOENT. posix_spawn,
 *     posix_spawnp, execve and execvp are interposed to retry an absolute
 *     system path under the prefix when, and only when, the original is
 *     missing and the prefixed one exists. Set NODEIOS_JB_PREFIX to override
 *     the default /var/jb.
 *
 *  5. #! scripts. This binary cannot exec one on a rootless jailbreak -- the
 *     kernel answers EPERM, or ENOENT for `#!/usr/bin/env` -- and every command
 *     npm installs is one. When a spawn or exec fails that way and the target
 *     is an executable #! script, the same four functions run its interpreter
 *     themselves, the way the kernel would have.
 */

#include <sys/mman.h>
#include <sys/ucontext.h>
#include <libkern/OSCacheControl.h>
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

static int g_debug = -1;

static int debug_on(void) {
    if (g_debug < 0) g_debug = getenv("NODEIOS_SHIM_DEBUG") != NULL;
    return g_debug;
}

/* Resolve the libSystem original of a symbol this file also defines. dlsym on an
   explicit libSystem handle, not RTLD_DEFAULT: the default search walks the
   global list and would find this shim's own definition first. */
static void *libsystem_handle(void) {
    static void *h = NULL;
    if (h == NULL) {
        h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY | RTLD_NOLOAD);
        if (h == NULL) h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    }
    return h;
}

static void *real_sym(const char *name, void *self) {
    void *h = libsystem_handle();
    void *p = h ? dlsym(h, name) : NULL;
    if (p == self) {
        fprintf(stderr, "[node-ios] fatal: %s resolved to the shim itself\n", name);
        abort();
    }
    return p;
}

/* ---- 1. JIT memory ----------------------------------------------------- */

/* V8's arm64 Darwin build reserves each isolate's code range with MAP_JIT and
   flips W^X through pthread_jit_write_protect_np(), which is per thread: the
   thread writing code sees its pages read-write while every other thread keeps
   executing them read-execute. iOS has neither. It rejects MAP_JIT (EINVAL),
   will not execute a page that is writable (SIGBUS), and refuses RWX even for a
   process jbctl has marked debugged -- but mprotect() may move a page between
   RW and RX, and a fault on the wrong one can be repaired and retried.

   This shim used to mprotect() every code range on every call. That is
   process-wide, so a worker isolate compiling pulled the main isolate's code
   out from under it: two CPU-heavy workers died with SIGBUS every time. So now
   nothing is flipped eagerly. Each page is RW or RX, and faults decide:

     write fault, thread inside a write scope   make that page RW, and count
                                                this thread as its writer
     execute fault                              wait until no thread is still
                                                writing the page, make it RX
     leaving the write scope                    drop this thread's writer
                                                counts; pages stay as they are

   The faulting instruction then runs again. Per thread, that is what the real
   API gives V8. Pages that are only ever executed stop faulting after their
   first run.

   V8 reserves a range with no access and grants access later with mprotect(),
   and unmaps the whole range when a worker isolate goes away. So mprotect and
   munmap are interposed as well, to keep the per-page record true: a request
   for RWX on a code page is granted as RW, which is what it can be on iOS, and
   an unmapped range stops being tracked.

   The fault handler has to run before V8's WebAssembly trap handler and Node's
   own, so it is installed from the constructor and sigaction() and signal() are
   interposed for SIGBUS and SIGSEGV: their handlers are recorded and chained
   to for every fault that is not one of ours. */

#define MAX_JIT_REGIONS 256
#define MAX_TOUCHED     512      /* pages one thread writes per scope, counted */

enum { PAGE_RW = 0, PAGE_RX = 1, PAGE_FOREIGN = 2 };

typedef struct {
    _Atomic uintptr_t start, end;  /* end == 0: slot free */
    size_t    npages, capacity;    /* capacity: pages the arrays below can hold */
    uint32_t  gen;                 /* bumped when the slot is reused */
    uint8_t  *state;               /* PAGE_*, authoritative while g_lock is held */
    uint16_t *writers;             /* threads in a write scope that wrote the page */
} jit_region_t;

typedef struct {
    int writing;
    int ntouched;
    struct { jit_region_t *r; uint32_t gen; size_t page; } touched[MAX_TOUCHED];
} writer_t;

static jit_region_t    g_regions[MAX_JIT_REGIONS];
static atomic_int      g_nregions;             /* slots ever used; only grows */
static atomic_flag     g_lock = ATOMIC_FLAG_INIT;
static pthread_mutex_t g_register_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t          g_page;
static pthread_key_t   g_writer_key;

static atomic_ulong    g_write_faults, g_exec_faults, g_waits, g_chained;

typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
typedef int   (*mprotect_fn)(void *, size_t, int);
typedef int   (*munmap_fn)(void *, size_t);
typedef int   (*sigaction_fn)(int, const struct sigaction *, struct sigaction *);

static mmap_fn      real_mmap;
static mprotect_fn  real_mprotect;
static munmap_fn    real_munmap;
static sigaction_fn real_sigaction;

/* Spin lock: taken inside a signal handler, where a mutex is not safe. Every
   critical section is a few assignments and at most one mprotect. */
static void jit_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire))
        sched_yield();
}
static void jit_unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

static jit_region_t *find_region(uintptr_t a, size_t *page) {
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n; i++) {
        jit_region_t *r = &g_regions[i];
        uintptr_t start = atomic_load(&r->start), end = atomic_load(&r->end);
        if (a >= start && a < end) {
            *page = (a - start) / g_page;
            return r;
        }
    }
    return NULL;
}

static uint8_t state_for_prot(int prot) {
    int wx = prot & (PROT_WRITE | PROT_EXEC);
    if (!(prot & PROT_READ)) return PAGE_FOREIGN;  /* PROT_NONE: reserved, not committed */
    if (wx == PROT_WRITE || wx == (PROT_WRITE | PROT_EXEC)) return PAGE_RW;
    if (wx == PROT_EXEC) return PAGE_RX;
    return PAGE_FOREIGN;
}

/* Record that [a, a+len) now has protection `prot`. */
static void note_protection(uintptr_t a, size_t len, int prot) {
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n; i++) {
        jit_region_t *r = &g_regions[i];
        uintptr_t start = atomic_load(&r->start), end = atomic_load(&r->end);
        uintptr_t lo = a > start ? a : start;
        uintptr_t hi = a + len < end ? a + len : end;
        if (lo >= hi) continue;
        size_t first = (lo - start) / g_page;
        size_t last  = (hi - start + g_page - 1) / g_page;
        uint8_t st = state_for_prot(prot);
        jit_lock();
        for (size_t p = first; p < last; p++) r->state[p] = st;
        jit_unlock();
    }
}

static int overlaps_region(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n; i++) {
        uintptr_t start = atomic_load(&g_regions[i].start), end = atomic_load(&g_regions[i].end);
        if (a < end && a + len > start) return 1;
    }
    return 0;
}

static void register_region(void *p, size_t len, int prot) {
    size_t dummy;
    if (find_region((uintptr_t)p, &dummy) != NULL) {   /* MAP_FIXED inside a range */
        note_protection((uintptr_t)p, len, prot);
        return;
    }
    size_t npages = (len + g_page - 1) / g_page;
    pthread_mutex_lock(&g_register_lock);
    jit_region_t *r = NULL;
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n && r == NULL; i++)             /* a free slot big enough */
        if (atomic_load(&g_regions[i].end) == 0 && g_regions[i].capacity >= npages)
            r = &g_regions[i];
    if (r == NULL) {
        if (n >= MAX_JIT_REGIONS) {
            pthread_mutex_unlock(&g_register_lock);
            fprintf(stderr, "[node-ios] fatal: more than %d JIT code ranges\n", MAX_JIT_REGIONS);
            abort();
        }
        r = &g_regions[n];
        size_t bytes = npages * (sizeof *r->state + sizeof *r->writers);
        void *mem = real_mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) {
            pthread_mutex_unlock(&g_register_lock);
            fprintf(stderr, "[node-ios] fatal: cannot allocate JIT page state\n");
            abort();
        }
        r->state    = mem;
        r->writers  = (uint16_t *)((uint8_t *)mem + npages);
        r->capacity = npages;
    }
    jit_lock();
    r->npages = npages;
    r->gen++;
    memset(r->state, state_for_prot(prot), npages);
    memset(r->writers, 0, npages * sizeof *r->writers);
    jit_unlock();
    atomic_store(&r->start, (uintptr_t)p);
    atomic_store(&r->end, (uintptr_t)p + npages * g_page);   /* live from here */
    if (r == &g_regions[n]) atomic_store(&g_nregions, n + 1);
    pthread_mutex_unlock(&g_register_lock);

    if (debug_on())
        fprintf(stderr, "[node-ios] JIT range %p len=%zu prot=0x%x\n", p, len, prot);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    if ((flags & MAP_JIT) == 0) {
        void *p = real_mmap(addr, len, prot, flags, fd, offset);
        if (p != MAP_FAILED && (flags & MAP_FIXED)) note_protection((uintptr_t)p, len, prot);
        return p;
    }
    /* A code range. RWX is never granted; RW is, and the first execution of
       each page makes it RX. */
    flags &= ~MAP_JIT;
    int initial = prot;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) initial = prot & ~PROT_EXEC;
    void *p = real_mmap(addr, len, initial, flags, fd, offset);
    if (p != MAP_FAILED) register_region(p, len, initial);
    return p;
}

int mprotect(void *addr, size_t len, int prot) {
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC) && overlaps_region((uintptr_t)addr, len))
        prot &= ~PROT_EXEC;
    int rc = real_mprotect(addr, len, prot);
    if (rc == 0) note_protection((uintptr_t)addr, len, prot);
    return rc;
}

int munmap(void *addr, size_t len) {
    int rc = real_munmap(addr, len);
    if (rc != 0) return rc;
    uintptr_t a = (uintptr_t)addr;
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n; i++) {
        jit_region_t *r = &g_regions[i];
        uintptr_t start = atomic_load(&r->start), end = atomic_load(&r->end);
        if (end == 0 || a >= end || a + len <= start) continue;
        if (a <= start && a + len >= end) {
            pthread_mutex_lock(&g_register_lock);
            atomic_store(&r->end, 0);                 /* not live from here */
            pthread_mutex_unlock(&g_register_lock);
            if (debug_on()) fprintf(stderr, "[node-ios] JIT range %p unmapped\n", (void *)start);
        } else {
            note_protection(a, len, PROT_NONE);
        }
    }
    return 0;
}

static writer_t *current_writer(void) { return pthread_getspecific(g_writer_key); }

static void check_handlers(void);

/* enabled=0: "about to write"; enabled=1: "done, must be executable again". */
void pthread_jit_write_protect_np(int enabled) {
    writer_t *w = current_writer();
    if (!enabled) {
        if (w == NULL) {
            /* Allocated here, in normal context, so the fault handler only ever
               reads it. */
            w = calloc(1, sizeof *w);
            if (w == NULL) abort();
            pthread_setspecific(g_writer_key, w);
        }
        w->writing = 1;
        check_handlers();
        return;
    }
    if (w == NULL || !w->writing) return;
    if (w->ntouched > 0) {
        jit_lock();
        for (int i = 0; i < w->ntouched; i++) {
            jit_region_t *r = w->touched[i].r;
            size_t pg = w->touched[i].page;
            if (r->gen == w->touched[i].gen && r->writers[pg] > 0) r->writers[pg]--;
        }
        jit_unlock();
        w->ntouched = 0;
    }
    w->writing = 0;
}

int pthread_jit_write_protect_supported_np(void) { return 1; }

/* A write to a code page. Only a thread inside a write scope may make one;
   anything else is a real bug and goes to the chained handler. */
static int repair_write(jit_region_t *r, size_t page) {
    writer_t *w = current_writer();
    if (w == NULL || !w->writing) return 0;
    atomic_fetch_add(&g_write_faults, 1);
    int ok = 1;
    jit_lock();
    if (page >= r->npages || r->state[page] == PAGE_FOREIGN) {
        ok = 0;
    } else {
        int seen = 0;
        for (int i = 0; i < w->ntouched; i++)
            if (w->touched[i].r == r && w->touched[i].page == page && w->touched[i].gen == r->gen) {
                seen = 1;
                break;
            }
        /* Past MAX_TOUCHED the page goes uncounted. Still correct -- every access
           faults and is repaired -- just liable to bounce between a writer and an
           executor until the scope ends. */
        if (!seen && w->ntouched < MAX_TOUCHED) {
            w->touched[w->ntouched].r    = r;
            w->touched[w->ntouched].gen  = r->gen;
            w->touched[w->ntouched].page = page;
            w->ntouched++;
            r->writers[page]++;
        }
        if (r->state[page] == PAGE_RX) {
            if (real_mprotect((void *)(atomic_load(&r->start) + page * g_page), g_page,
                              PROT_READ | PROT_WRITE) == 0)
                r->state[page] = PAGE_RW;
            else
                ok = 0;
        }
    }
    jit_unlock();
    return ok;
}

/* An execution of a code page. Waits out any thread still writing it. */
static int repair_exec(jit_region_t *r, size_t page) {
    writer_t *w = current_writer();
    /* On macOS a thread inside its own write scope cannot execute JIT code
       either. Crash the same way rather than wait on ourselves forever. */
    if (w != NULL && w->writing) return 0;
    atomic_fetch_add(&g_exec_faults, 1);
    for (unsigned spins = 0;; spins++) {
        jit_lock();
        if (page >= r->npages || r->state[page] == PAGE_FOREIGN) { jit_unlock(); return 0; }
        if (r->writers[page] == 0) {
            int ok = 1;
            if (r->state[page] == PAGE_RW) {
                if (real_mprotect((void *)(atomic_load(&r->start) + page * g_page), g_page,
                                  PROT_READ | PROT_EXEC) == 0)
                    r->state[page] = PAGE_RX;
                else
                    ok = 0;
            }
            jit_unlock();
            return ok;
        }
        jit_unlock();
        if (spins == 0) atomic_fetch_add(&g_waits, 1);
        if (spins < 64) {
            sched_yield();
        } else {
            struct timespec ts = { 0, 50000 };   /* 50 us */
            nanosleep(&ts, NULL);
        }
    }
}

/* ---- fault handler ordering ------------------------------------------- */

static struct sigaction g_app[2];          /* SIGBUS, SIGSEGV as the program set them */
static atomic_flag      g_sig_lock = ATOMIC_FLAG_INIT;

static struct sigaction *app_slot(int sig) {
    return sig == SIGBUS ? &g_app[0] : sig == SIGSEGV ? &g_app[1] : NULL;
}
static void sig_lock(void)   { while (atomic_flag_test_and_set(&g_sig_lock)) sched_yield(); }
static void sig_unlock(void) { atomic_flag_clear(&g_sig_lock); }

static void chain(int sig, siginfo_t *si, void *ctx) {
    atomic_fetch_add(&g_chained, 1);
    sig_lock();
    struct sigaction app = *app_slot(sig);
    if (app.sa_flags & SA_RESETHAND) {
        memset(app_slot(sig), 0, sizeof app);
        app_slot(sig)->sa_handler = SIG_DFL;
    }
    sig_unlock();

    int has_info = (app.sa_flags & SA_SIGINFO) != 0;
    if ((has_info && app.sa_sigaction != NULL) ||
        (!has_info && app.sa_handler != SIG_DFL && app.sa_handler != SIG_IGN)) {
        sigset_t saved;
        pthread_sigmask(SIG_BLOCK, &app.sa_mask, &saved);
        if (has_info) app.sa_sigaction(sig, si, ctx);
        else          app.sa_handler(sig);
        pthread_sigmask(SIG_SETMASK, &saved, NULL);
        return;
    }
    /* Default action: restore it for real and raise the signal again. It stays
       pending until this handler returns, then kills the process the way it
       would have without the shim. Returning alone is not enough: a signal
       that was sent rather than faulted -- kill(), or Node resetting its
       handler and calling raise() -- would never arrive a second time. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    real_sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void jit_fault(int sig, siginfo_t *si, void *ctx) {
    uintptr_t far = (uintptr_t)si->si_addr;
    size_t page;
    jit_region_t *r = find_region(far, &page);
    if (r != NULL) {
        ucontext_t *uc = ctx;
        uint32_t esr = uc->uc_mcontext->__es.__esr;
        uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        unsigned ec = esr >> 26;
        int exec  = ec == 0x20 || ec == 0x21 || pc == far;          /* instruction abort */
        int write = !exec && (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));  /* data abort, WnR */
        if (exec  && repair_exec(r, page))  return;
        if (write && repair_write(r, page)) return;
    }
    chain(sig, si, ctx);
}

static void install_handler(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = jit_fault;
    /* SA_ONSTACK so that a chained handler still gets the alternate stack it
       asked for, which is what makes a stack overflow reportable. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    real_sigaction(sig, &sa, NULL);
}

/* Belt and braces for the interposition: if a handler was installed past it,
   adopt that one as the chained handler and put ours back in front. Cheap
   enough to run on the first write scopes and then every few thousand. */
static void check_handlers(void) {
    static atomic_ulong calls;
    unsigned long c = atomic_fetch_add(&calls, 1);
    if (c > 16 && (c & 4095) != 0) return;
    static const int sigs[2] = { SIGBUS, SIGSEGV };
    for (int i = 0; i < 2; i++) {
        struct sigaction cur;
        if (real_sigaction(sigs[i], NULL, &cur) != 0) continue;
        if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == jit_fault) continue;
        sig_lock();
        *app_slot(sigs[i]) = cur;
        sig_unlock();
        install_handler(sigs[i]);
        if (debug_on()) fprintf(stderr, "[node-ios] reinstalled fault handler for %d\n", sigs[i]);
    }
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    struct sigaction *slot = app_slot(sig);
    if (slot == NULL) return real_sigaction(sig, act, oact);
    sig_lock();
    if (oact != NULL) *oact = *slot;
    if (act != NULL)  *slot = *act;
    sig_unlock();
    return 0;
}

void (*signal(int sig, void (*func)(int)))(int) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = func;
    sa.sa_flags = SA_RESTART;                    /* BSD signal() semantics */
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, &old) != 0) return SIG_ERR;
    return (old.sa_flags & SA_SIGINFO) ? (void (*)(int))(uintptr_t)old.sa_sigaction
                                       : old.sa_handler;
}

static void report_counters(void) {
    fprintf(stderr, "[node-ios] JIT faults: write %lu, exec %lu, waited %lu, chained %lu\n",
            atomic_load(&g_write_faults), atomic_load(&g_exec_faults),
            atomic_load(&g_waits), atomic_load(&g_chained));
}

/* Runs before section 6's constructor sets V8's flags, and before main. */
__attribute__((constructor(101)))
static void nodeios_jit_init(void) {
    real_mmap      = (mmap_fn)real_sym("mmap", (void *)mmap);
    real_mprotect  = (mprotect_fn)real_sym("mprotect", (void *)mprotect);
    real_munmap    = (munmap_fn)real_sym("munmap", (void *)munmap);
    real_sigaction = (sigaction_fn)real_sym("sigaction", (void *)sigaction);
    if (!real_mmap || !real_mprotect || !real_munmap || !real_sigaction) {
        fprintf(stderr, "[node-ios] fatal: cannot resolve libSystem's memory or signal calls\n");
        abort();
    }
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    if (pthread_key_create(&g_writer_key, free) != 0) abort();

    real_sigaction(SIGBUS,  NULL, &g_app[0]);
    real_sigaction(SIGSEGV, NULL, &g_app[1]);
    install_handler(SIGBUS);
    install_handler(SIGSEGV);

    if (debug_on()) atexit(report_counters);
}

/* ---- 2. macOS-only Security API ---------------------------------------- */

/* errSecNoTrustSettings: "no trust settings were found for this certificate",
   the same answer a macOS box with an untouched keychain gives. Node treats it
   as "nothing to add" and keeps its bundled roots. */
#define ERR_SEC_NO_TRUST_SETTINGS (-25263)

int SecTrustSettingsCopyTrustSettings(void *certRef, unsigned int domain,
                                      void **trustSettings) {
    (void)certRef; (void)domain;
    if (trustSettings) *trustSettings = NULL;
    return ERR_SEC_NO_TRUST_SETTINGS;
}

/* ---- 3. syslog$DARWIN_EXTSN -------------------------------------------- */

void syslog_darwin_extsn(int priority, const char *format, ...) __asm__("_syslog$DARWIN_EXTSN");

void syslog_darwin_extsn(int priority, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

/* ---- 4. exec path remapping -------------------------------------------- */

/* Only these roots are remapped. They are the ones a rootless bootstrap
   shadows; anything else is left exactly as the caller asked for. */
static const char *const REMAP_ROOTS[] = {
    "/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/", "/usr/local/bin/", NULL
};

static const char *jb_prefix(void) {
    static const char *p = NULL;
    if (p == NULL) {
        p = getenv("NODEIOS_JB_PREFIX");
        if (p == NULL || *p == '\0') p = "/var/jb";
    }
    return p;
}

/* Returns `buf` holding the prefixed path when `path` is an absolute system
   path that does not exist but does exist under the prefix; otherwise NULL and
   the caller uses the original. */
static const char *remap_exec(const char *path, char *buf, size_t buflen) {
    if (path == NULL || path[0] != '/') return NULL;

    int rooted = 0;
    for (int i = 0; REMAP_ROOTS[i]; i++) {
        size_t n = strlen(REMAP_ROOTS[i]);
        if (strncmp(path, REMAP_ROOTS[i], n) == 0) { rooted = 1; break; }
    }
    if (!rooted) return NULL;

    struct stat st;
    if (stat(path, &st) == 0) return NULL;   /* the real thing is there */

    if ((size_t)snprintf(buf, buflen, "%s%s", jb_prefix(), path) >= buflen)
        return NULL;
    if (stat(buf, &st) != 0) return NULL;    /* no better answer to give */

    if (debug_on())
        fprintf(stderr, "[node-ios] exec remap %s -> %s\n", path, buf);
    return buf;
}

typedef int (*posix_spawn_fn)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *,
                              char *const [], char *const []);
typedef int (*execve_fn)(const char *, char *const [], char *const []);
typedef int (*execvp_fn)(const char *, char *const []);

int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *,
                const posix_spawnattr_t *, char *const [], char *const []);
int execve(const char *, char *const [], char *const []);

static posix_spawn_fn real_posix_spawn(void) {
    static posix_spawn_fn real = NULL;
    if (real == NULL) real = (posix_spawn_fn)real_sym("posix_spawn", (void *)posix_spawn);
    return real;
}

static execve_fn real_execve(void) {
    static execve_fn real = NULL;
    if (real == NULL) real = (execve_fn)real_sym("execve", (void *)execve);
    return real;
}

/* ---- 5. #! scripts ----------------------------------------------------- */

/* Measured on iOS 17.3 / Dopamine, node spawning an executable script:

     #!/var/jb/usr/bin/env node    EPERM
     #!/var/jb/usr/bin/zsh         EPERM
     #!/usr/bin/env node           ENOENT   (no /usr/bin/env outside /var/jb)

   The same scripts run when zsh or python3 spawns them -- the jailbreak does
   the #! handling for its own bootstrap binaries in userland -- and a signed
   Mach-O runs from anywhere. npm itself, and every command `npm install -g`
   creates, is such a script, so without this a node program could spawn none
   of them by name.

   So when a spawn or exec fails with one of those errors and the target is an
   executable #! script, do what the kernel would have: exec the interpreter
   with the optional argument, the script's path, and the original arguments
   after argv[0]. The interpreter goes through remap_exec, which is what turns
   /usr/bin/env into /var/jb/usr/bin/env. Only after a failure, only for a
   regular file with an execute bit that really starts with #!, and only one
   level deep -- nothing that works today takes a different path.

   Everything here may run in a forked child between fork and exec (libuv's
   fallback when posix_spawn cannot express the request), so it sticks to
   stack buffers and open/read/stat/access, with no allocation. */

#define SHEBANG_MAX      512      /* XNU's own limit for a #! line */
#define SHEBANG_MAX_ARGS 65536    /* bounds the argv array on the stack */

typedef struct {
    char        interp[SHEBANG_MAX];    /* as written in the script */
    char        arg[SHEBANG_MAX];       /* "" when there is none */
    char        remapped[1024];
    const char *exec_path;              /* interp, or its /var/jb stand-in */
} shebang_t;

static int shebang_errno(int err) {
    return err == EPERM || err == ENOENT || err == ENOEXEC;
}

static int read_shebang(const char *path, shebang_t *sb) {
    struct stat st;
    if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (access(path, X_OK) != 0) return 0;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char line[SHEBANG_MAX + 1];
    ssize_t n = read(fd, line, SHEBANG_MAX);
    close(fd);
    if (n < 3 || line[0] != '#' || line[1] != '!') return 0;

    char *end = memchr(line, '\n', (size_t)n);
    if (end == NULL) return 0;              /* over-long: the kernel refuses too */
    while (end > line + 2 && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    *end = '\0';

    char *p = line + 2;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '/') return 0;                /* relative or missing interpreter */
    char *q = p;
    while (*q != '\0' && *q != ' ' && *q != '\t') q++;
    memcpy(sb->interp, p, (size_t)(q - p));
    sb->interp[q - p] = '\0';

    while (*q == ' ' || *q == '\t') q++;    /* the rest of the line is one argument */
    memcpy(sb->arg, q, strlen(q) + 1);

    const char *r = remap_exec(sb->interp, sb->remapped, sizeof sb->remapped);
    sb->exec_path = r ? r : sb->interp;
    return 1;
}

static size_t arg_count(char *const argv[]) {
    size_t n = 0;
    if (argv) while (argv[n]) n++;
    return n;
}

/* interp [arg] script argv[1..]; `out` must hold arg_count(argv) + 4 slots. */
static void shebang_argv(const shebang_t *sb, const char *script,
                         char *const argv[], size_t n, char **out) {
    size_t i = 0;
    out[i++] = (char *)sb->interp;
    if (sb->arg[0] != '\0') out[i++] = (char *)sb->arg;
    out[i++] = (char *)script;
    for (size_t k = 1; k < n; k++) out[i++] = argv[k];
    out[i] = NULL;
}

/* First executable regular file named `file` on $PATH, as execvp and
   posix_spawnp would pick it. NULL when there is none or it does not fit. */
static const char *search_path(const char *file, char *buf, size_t buflen) {
    const char *path = getenv("PATH");
    if (path == NULL) path = "/usr/bin:/bin";
    size_t flen = strlen(file);
    for (const char *p = path;; ) {
        const char *z = strchr(p, ':');
        size_t dlen = z ? (size_t)(z - p) : strlen(p);
        if (dlen == 0) {
            if (flen + 1 <= buflen) memcpy(buf, file, flen + 1);   /* empty entry: cwd */
            else goto next;
        } else if (dlen + 1 + flen + 1 <= buflen) {
            memcpy(buf, p, dlen);
            buf[dlen] = '/';
            memcpy(buf + dlen + 1, file, flen + 1);
        } else {
            goto next;
        }
        {
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0)
                return buf;
        }
    next:
        if (z == NULL) return NULL;
        p = z + 1;
    }
}

/* posix_spawn(p) failed with `err`; retry through the script's interpreter.
   Returns `err` untouched when the target is not a script we can run. */
static int spawn_script(pid_t *pid, const char *script,
                        const posix_spawn_file_actions_t *acts,
                        const posix_spawnattr_t *attr,
                        char *const argv[], char *const envp[], int err) {
    shebang_t sb;
    if (!shebang_errno(err) || !read_shebang(script, &sb)) return err;
    size_t n = arg_count(argv);
    if (n > SHEBANG_MAX_ARGS) return err;
    char *args[n + 4];
    shebang_argv(&sb, script, argv, n, args);
    if (debug_on())
        fprintf(stderr, "[node-ios] #! %s -> %s\n", script, sb.exec_path);
    return real_posix_spawn()(pid, sb.exec_path, acts, attr, args, envp);
}

/* execve/execvp failed with `err`; retry through the interpreter. Returns
   only on failure, with the errno to report. */
static int exec_script(const char *script, char *const argv[], char *const envp[], int err) {
    shebang_t sb;
    if (!shebang_errno(err) || !read_shebang(script, &sb)) return err;
    size_t n = arg_count(argv);
    if (n > SHEBANG_MAX_ARGS) return err;
    char *args[n + 4];
    shebang_argv(&sb, script, argv, n, args);
    if (debug_on())
        fprintf(stderr, "[node-ios] #! %s -> %s\n", script, sb.exec_path);
    real_execve()(sb.exec_path, args, envp);
    return errno;
}

/* ---- 4 and 5: the interposed calls ------------------------------------- */

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *acts,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    char buf[1024];
    const char *p = remap_exec(path, buf, sizeof buf);
    const char *target = p ? p : path;
    int err = real_posix_spawn()(pid, target, acts, attr, argv, envp);
    if (err == 0) return 0;
    return spawn_script(pid, target, acts, attr, argv, envp, err);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *acts,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    static posix_spawn_fn real = NULL;
    if (real == NULL) real = (posix_spawn_fn)real_sym("posix_spawnp", (void *)posix_spawnp);
    char buf[1024];
    const char *p = remap_exec(file, buf, sizeof buf);
    const char *target = p ? p : file;
    int err = real(pid, target, acts, attr, argv, envp);
    if (err == 0 || target == NULL || !shebang_errno(err)) return err;

    char found[1024];
    const char *script = strchr(target, '/') ? target
                                             : search_path(target, found, sizeof found);
    if (script == NULL) return err;
    return spawn_script(pid, script, acts, attr, argv, envp, err);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    char buf[1024];
    const char *p = remap_exec(path, buf, sizeof buf);
    const char *target = p ? p : path;
    real_execve()(target, argv, envp);
    errno = exec_script(target, argv, envp, errno);
    return -1;
}

int execvp(const char *file, char *const argv[]) {
    static execvp_fn real = NULL;
    if (real == NULL) real = (execvp_fn)real_sym("execvp", (void *)execvp);
    char buf[1024];
    const char *p = remap_exec(file, buf, sizeof buf);
    const char *target = p ? p : file;
    real(target, argv);
    int err = errno;
    if (target == NULL || !shebang_errno(err)) return -1;

    char found[1024];
    const char *script = strchr(target, '/') ? target
                                             : search_path(target, found, sizeof found);
    if (script != NULL) err = exec_script(script, argv, *_NSGetEnviron(), err);
    errno = err;
    return -1;
}

/* ---- 6. force the V8 flags the W^X emulation requires ------------------ */

/* The flip above is process-wide where the real pthread_jit_write_protect_np is
   per-thread, so a background compiler thread can swing the pool to RW while
   the main thread is executing out of it -- an intermittent SIGBUS. V8 must
   therefore run --single-threaded, and that flag is rejected in NODE_OPTIONS,
   so it cannot simply be exported into the environment for child processes.
   Node exports v8::V8::SetFlagsFromString, so set it here instead: a dylib
   constructor runs before main and long before V8::Initialize, and this way
   *every* way of reaching the binary is covered -- the wrapper, a
   `#!/usr/bin/env node` shebang, and anything npm spawns via process.execPath.
   Set NODEIOS_V8_FLAGS to override, or to the empty string to opt out. */
__attribute__((constructor))
static void nodeios_set_v8_flags(void) {
    const char *flags = getenv("NODEIOS_V8_FLAGS");
    if (flags == NULL) flags = "--single-threaded";
    if (*flags == '\0') return;

    void (*set_flags)(const char *) =
        (void (*)(const char *))dlsym(RTLD_DEFAULT,
                                      "_ZN2v82V818SetFlagsFromStringEPKc");
    if (set_flags == NULL) {
        fprintf(stderr, "[node-ios] warning: v8::V8::SetFlagsFromString not found; "
                        "JIT will race and crash. Pass --single-threaded by hand.\n");
        return;
    }
    set_flags(flags);
    if (debug_on()) fprintf(stderr, "[node-ios] V8 flags forced: %s\n", flags);
}
