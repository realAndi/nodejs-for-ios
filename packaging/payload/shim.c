/*  node-ios shim -- lets the official macOS arm64 Node.js binary run on iOS.
 *
 *  Node reaches these through patched LC_DYLD_CHAINED_FIXUPS import ordinals
 *  (nodeios_patch.py), not DYLD_INSERT_LIBRARIES: dyld ignores DYLD_* for an
 *  entitled binary, and the two-level import table would never pick an
 *  inserted library anyway.
 *
 *  Three of Node's 725 imports are absent from iOS's libraries, plus mmap is
 *  interposed for JIT:
 *
 *  1. pthread_jit_write_protect_np -- macOS-only (SDK marks it
 *     __API_UNAVAILABLE(ios); absent on iOS 17.3). V8's arm64 Darwin build
 *     maps its code pages MAP_JIT and flips per-thread W^X through this call.
 *     iOS rejects MAP_JIT (EINVAL) and SIGBUSes on executing a writable page,
 *     but mprotect() may swing a page RW<->RX. So: strip MAP_JIT in mmap,
 *     remember the region, and implement the toggle as mprotect over every
 *     remembered region. That is process-wide where the real API is
 *     per-thread, so node must run --single-threaded (or at least with
 *     concurrent recompilation off).
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
 */

#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

#define MAX_JIT_REGIONS 256

static struct { void *addr; size_t len; } g_jit[MAX_JIT_REGIONS];

/* Whether *this* thread is currently inside a write scope. V8 enters the scope
   and only then maps the code range it is about to write into, so a region
   mapped while the flag is set must come up read-write or the very next store
   faults. */
static _Thread_local int t_write_scope = 0;
static int             g_jit_count = 0;
static pthread_mutex_t g_jit_lock  = PTHREAD_MUTEX_INITIALIZER;
static int             g_debug     = -1;

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

typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    static mmap_fn real = NULL;
    if (real == NULL) real = (mmap_fn)real_sym("mmap", (void *)mmap);

    if ((flags & MAP_JIT) == 0)
        return real(addr, len, prot, flags, fd, offset);

    /* V8's executable pool. Map it RW now; the toggle below swings it to RX
       once V8 is ready to run what it wrote. */
    flags &= ~MAP_JIT;
    int initial = prot;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) initial = prot & ~PROT_EXEC;
    if (t_write_scope) initial = PROT_READ | PROT_WRITE;

    void *p = real(addr, len, initial, flags, fd, offset);
    if (p == MAP_FAILED) return p;

    pthread_mutex_lock(&g_jit_lock);
    if (g_jit_count < MAX_JIT_REGIONS) {
        g_jit[g_jit_count].addr  = p;
        g_jit[g_jit_count].len   = len;
        g_jit_count++;
    }
    pthread_mutex_unlock(&g_jit_lock);

    if (debug_on())
        fprintf(stderr, "[node-ios] JIT pool %p len=%zu prot=0x%x->0x%x (%d tracked)\n",
                p, len, prot, initial, g_jit_count);
    return p;
}

/* enabled=0: "about to write"; enabled=1: "done, must be executable again".

   Only the calling thread's own regions are flipped. mprotect is process-wide
   while the API it stands in for is per-thread, so flipping every region would
   make a worker isolate's compile step unmap the main isolate's executable
   pages mid-instruction -- an intermittent SIGBUS that scales with the worker
   count. Each isolate gets its own code range, allocated on the thread that
   created it, so owner-filtering restores the per-thread semantics V8 expects.
   If the caller owns nothing (a thread writing a range it did not map), fall
   back to flipping everything: slower and racy, but never wrong-by-omission.

   A region can be unmapped while still listed (isolate teardown), after which
   mprotect fails harmlessly -- drop it rather than abort. */
void pthread_jit_write_protect_np(int enabled) {
    int prot = enabled ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);

    t_write_scope = !enabled;

    pthread_mutex_lock(&g_jit_lock);
    for (int i = 0; i < g_jit_count; i++) {
        if (mprotect(g_jit[i].addr, g_jit[i].len, prot) != 0) {
            if (debug_on())
                fprintf(stderr, "[node-ios] mprotect(%p, %zu, 0x%x) failed: %d -- dropping\n",
                        g_jit[i].addr, g_jit[i].len, prot, errno);
            g_jit[i] = g_jit[--g_jit_count];
            i--;
        }
    }
    pthread_mutex_unlock(&g_jit_lock);
}

int pthread_jit_write_protect_supported_np(void) { return 1; }

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

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *acts,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    static posix_spawn_fn real = NULL;
    if (real == NULL) real = (posix_spawn_fn)real_sym("posix_spawn", (void *)posix_spawn);
    char buf[1024];
    const char *p = remap_exec(path, buf, sizeof buf);
    return real(pid, p ? p : path, acts, attr, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *acts,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    static posix_spawn_fn real = NULL;
    if (real == NULL) real = (posix_spawn_fn)real_sym("posix_spawnp", (void *)posix_spawnp);
    char buf[1024];
    const char *p = remap_exec(file, buf, sizeof buf);
    return real(pid, p ? p : file, acts, attr, argv, envp);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    static execve_fn real = NULL;
    if (real == NULL) real = (execve_fn)real_sym("execve", (void *)execve);
    char buf[1024];
    const char *p = remap_exec(path, buf, sizeof buf);
    return real(p ? p : path, argv, envp);
}

int execvp(const char *file, char *const argv[]) {
    static execvp_fn real = NULL;
    if (real == NULL) real = (execvp_fn)real_sym("execvp", (void *)execvp);
    char buf[1024];
    const char *p = remap_exec(file, buf, sizeof buf);
    return real(p ? p : file, argv);
}

/* ---- 5. force the V8 flags the W^X emulation requires ------------------ */

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
