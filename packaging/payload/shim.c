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
 *
 *  5. #! scripts. This binary cannot exec one on a rootless jailbreak -- the
 *     kernel answers EPERM, or ENOENT for `#!/usr/bin/env` -- and every command
 *     npm installs is one. When a spawn or exec fails that way and the target
 *     is an executable #! script, the same four functions run its interpreter
 *     themselves, the way the kernel would have.
 */

#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
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
