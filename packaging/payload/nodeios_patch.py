#!/usr/bin/env python3
"""Patch an official macOS arm64 Node.js binary so it runs on jailbroken iOS.

Pure stdlib and no cctools dependency, so it can also run on the phone
(Procursus ships ldid but not vtool):

  1. LC_BUILD_VERSION platform -> iOS                    (replaces `vtool`)
  2. rewrite the CoreFoundation/Security LC_LOAD_DYLIB paths from the macOS
     ".framework/Versions/A/Foo" layout to the flat iOS one
  3. append LC_LOAD_DYLIB for the shim and repoint the imports it provides,
     inside LC_DYLD_CHAINED_FIXUPS
  4. rewrite any ".framework/Versions/X/Foo" dlopen path left in the data

Patching is idempotent. A binary patched by an earlier version already links
the shim; its load command is reused and only the imports this version adds
are repointed, so a newer shim never runs beside a binary that bypasses it.

  nodeios_patch.py <in> <out> [--shim @executable_path/libnodeshim.dylib]
  nodeios_patch.py <in> --check          # report only, write nothing
  nodeios_patch.py <in> --current        # exit 0 only if nothing is left to patch

Exit status is non-zero if the binary is not one we know how to patch, so the
build can refuse to ship it.
"""
import struct
import sys

LC_SEGMENT_64           = 0x19
LC_LOAD_DYLIB           = 0x0C
LC_VERSION_MIN_MACOSX   = 0x24
LC_VERSION_MIN_IPHONEOS = 0x25
LC_BUILD_VERSION        = 0x32
LC_DYLD_CHAINED_FIXUPS  = 0x80000034

PLATFORM_IOS   = 2
MH_MAGIC_64    = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C

IOS_MINOS = (15, 0, 0)
IOS_SDK   = (17, 0, 0)

# Without these nothing runs: mmap is interposed so MAP_JIT can be stripped
# and the code range remembered, and pthread_jit_write_protect_np (absent from
# iOS altogether) is what V8 calls around every write to code. Their absence
# means the binary is not a V8 build we understand.
REQUIRED = [
    "_mmap",
    "_pthread_jit_write_protect_np",
    # The shim repairs JIT page faults instead of flipping whole code ranges,
    # which is what makes worker threads safe. That needs its SIGBUS/SIGSEGV
    # handler to stay in front of V8's and Node's (sigaction), and a true record
    # of every code page's protection: V8 reserves ranges with no access and
    # grants it with mprotect, and unmaps a worker's range when it exits. Without
    # any one of these repointed, the first execution of JIT code would crash.
    "_sigaction",
    "_mprotect",
    "_munmap",
]

# Repointed when present, skipped when not. Which of these a given Node build
# imports varies by version -- 24.21.0 has no posix_spawnp where 26.8.1 does --
# so requiring them would reject perfectly patchable binaries.
OPTIONAL = [
    # macOS-only, genuinely absent from iOS.
    "_SecTrustSettingsCopyTrustSettings",
    "_syslog$DARWIN_EXTSN",
    # std::__libcpp_verbose_abort: iOS 16's libc++ exports it, iOS 15's does
    # not, and dyld refuses to start Node there without it.
    "__ZNSt3__122__libcpp_verbose_abortEPKcz",
    # present on iOS; interposed so a rootless bootstrap's /var/jb paths are
    # found when Node asks for /bin/sh and friends.
    "_posix_spawn",
    "_posix_spawnp",
    "_execve",
    "_execvp",
    # signal() would otherwise install a SIGBUS/SIGSEGV handler past the
    # interposed sigaction().
    "_signal",
]

SHIMMED = REQUIRED + OPTIONAL

# macOS framework load paths that exist on iOS only in the flat layout.
FRAMEWORK_REWRITES = {
    "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation":
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
    "/System/Library/Frameworks/Security.framework/Versions/A/Security":
        "/System/Library/Frameworks/Security.framework/Security",
}

# Anything linked outside this set may not exist on iOS -> refuse.
ALLOWED_DYLIBS = tuple(FRAMEWORK_REWRITES) + tuple(FRAMEWORK_REWRITES.values()) + (
    "/usr/lib/libc++.1.dylib",
    "/usr/lib/libSystem.B.dylib",
)


def enc_version(v):
    return (v[0] << 16) | (v[1] << 8) | v[2]


class MachO(object):
    def __init__(self, data):
        self.d = bytearray(data)
        magic = struct.unpack_from("<I", self.d, 0)[0]
        if magic != MH_MAGIC_64:
            raise SystemExit("not a 64-bit little-endian Mach-O "
                             "(fat binary? thin it with lipo first)")
        cputype = struct.unpack_from("<i", self.d, 4)[0]
        if cputype != CPU_TYPE_ARM64:
            raise SystemExit("cputype 0x%x is not arm64" % (cputype & 0xFFFFFFFF))
        self.ncmds, self.sizeofcmds = struct.unpack_from("<II", self.d, 16)

    def commands(self):
        off = 32
        for _ in range(self.ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.d, off)
            yield off, cmd, cmdsize
            off += cmdsize

    def dylibs(self):
        """[(name, load-command offset, name offset within it, cmdsize)]"""
        out = []
        for off, cmd, cmdsize in self.commands():
            if cmd == LC_LOAD_DYLIB:
                noff = struct.unpack_from("<I", self.d, off + 8)[0]
                name = self.d[off + noff:off + cmdsize].split(b"\0")[0].decode()
                out.append((name, off, noff, cmdsize))
        return out

    def text_start(self):
        """Lowest file offset of a __TEXT section = end of usable header space."""
        low = None
        for off, cmd, cmdsize in self.commands():
            if cmd != LC_SEGMENT_64:
                continue
            if self.d[off + 8:off + 24].rstrip(b"\0") != b"__TEXT":
                continue
            nsects = struct.unpack_from("<I", self.d, off + 64)[0]
            for i in range(nsects):
                fo = struct.unpack_from("<I", self.d, off + 72 + i * 80 + 48)[0]
                if fo and (low is None or fo < low):
                    low = fo
        return low

    # --- 1. platform ----------------------------------------------------
    def set_ios_platform(self):
        done = False
        for off, cmd, cmdsize in self.commands():
            if cmd == LC_BUILD_VERSION:
                old = struct.unpack_from("<I", self.d, off + 8)[0]
                struct.pack_into("<I", self.d, off + 8, PLATFORM_IOS)
                struct.pack_into("<I", self.d, off + 12, enc_version(IOS_MINOS))
                struct.pack_into("<I", self.d, off + 16, enc_version(IOS_SDK))
                print("  LC_BUILD_VERSION platform %d -> %d (iOS), minos %d.%d sdk %d.%d"
                      % (old, PLATFORM_IOS, IOS_MINOS[0], IOS_MINOS[1],
                         IOS_SDK[0], IOS_SDK[1]))
                done = True
            elif cmd == LC_VERSION_MIN_MACOSX:
                struct.pack_into("<I", self.d, off, LC_VERSION_MIN_IPHONEOS)
                print("  LC_VERSION_MIN_MACOSX -> LC_VERSION_MIN_IPHONEOS")
                done = True
        if not done:
            raise SystemExit("no LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX to patch")

    # --- 2. framework load paths ----------------------------------------
    def rewrite_framework_loads(self):
        """Rewrite in place. Every replacement is shorter than the original, so
        the load command keeps its size and the tail is re-padded with NULs."""
        for name, off, noff, cmdsize in self.dylibs():
            new = FRAMEWORK_REWRITES.get(name)
            if new is None:
                continue
            nb = new.encode()
            room = cmdsize - noff
            if len(nb) + 1 > room:
                raise SystemExit("no room to rewrite %s" % name)
            self.d[off + noff:off + cmdsize] = nb + b"\0" * (room - len(nb))
            print("  LC_LOAD_DYLIB %s\n                -> %s" % (name, new))

    # --- 3. chained-fixup imports ---------------------------------------
    def chained_fixups(self):
        for off, cmd, cmdsize in self.commands():
            if cmd == LC_DYLD_CHAINED_FIXUPS:
                return struct.unpack_from("<II", self.d, off + 8)
        raise SystemExit("no LC_DYLD_CHAINED_FIXUPS (unexpected for this build)")

    def find_imports(self, names):
        dataoff, _ = self.chained_fixups()
        (_, _, imports_off, symbols_off,
         imports_count, imports_fmt, _) = struct.unpack_from("<7I", self.d, dataoff)
        if imports_fmt != 1:
            raise SystemExit("imports_format %d not handled" % imports_fmt)
        base, sym_base = dataoff + imports_off, dataoff + symbols_off
        found = {}
        for i in range(imports_count):
            v = struct.unpack_from("<I", self.d, base + i * 4)[0]
            name_off = v >> 9
            end = self.d.index(b"\0", sym_base + name_off)
            name = self.d[sym_base + name_off:end].decode()
            if name in names:
                found[name] = (i, v & 0xFF, (v >> 8) & 1, name_off)
        return base, found

    def shim_ordinal(self, shim_path):
        for i, (name, _, _, _) in enumerate(self.dylibs(), 1):
            if name == shim_path:
                return i
        return None

    def unshimmed(self, shim_path):
        """Imports this version routes to the shim that still go elsewhere."""
        ordinal = self.shim_ordinal(shim_path)
        _, found = self.find_imports(set(SHIMMED))
        return [n for n in SHIMMED if n in found and found[n][1] != ordinal]

    def add_shim(self, shim_path):
        base, found = self.find_imports(set(SHIMMED))
        missing = [n for n in REQUIRED if n not in found]
        if missing:
            raise SystemExit("required symbols absent from the import table: %s\n"
                             "upstream changed what it imports -- review shim.c"
                             % ", ".join(missing))

        path = shim_path.encode() + b"\0"
        cmdsize = (24 + len(path) + 7) & ~7
        slack = self.text_start() - (32 + self.sizeofcmds)
        if self.shim_ordinal(shim_path) is None and cmdsize > slack:
            raise SystemExit("no header room for LC_LOAD_DYLIB: need %d, have %d"
                             % (cmdsize, slack))

        new_ord = self.shim_ordinal(shim_path)
        if new_ord is not None:
            print("  = LC_LOAD_DYLIB ordinal %d -> %s (already linked)" % (new_ord, shim_path))
        else:
            new_ord = len(self.dylibs()) + 1
            ins = 32 + self.sizeofcmds
            lc = struct.pack("<IIIIII", LC_LOAD_DYLIB, cmdsize, 24, 0, 0x10000, 0x10000)
            lc += path + b"\0" * (cmdsize - 24 - len(path))
            self.d[ins:ins + cmdsize] = lc
            self.ncmds += 1
            self.sizeofcmds += cmdsize
            struct.pack_into("<II", self.d, 16, self.ncmds, self.sizeofcmds)
            print("  + LC_LOAD_DYLIB ordinal %d -> %s (%d bytes, %d slack left)"
                  % (new_ord, shim_path, cmdsize, slack - cmdsize))

        for name in SHIMMED:
            if name not in found:
                print("    (skipped %s -- not imported by this build)" % name)
                continue
            i, lib_ord, weak, name_off = found[name]
            if lib_ord == new_ord:
                continue
            struct.pack_into("<I", self.d, base + i * 4,
                             (new_ord & 0xFF) | (weak << 8) | (name_off << 9))
            print("    import[%d] %s: ordinal %d -> %d" % (i, name, lib_ord, new_ord))

    # --- 4. framework dlopen strings ------------------------------------
    def fix_framework_paths(self):
        needle, n, i = b".framework/Versions/", 0, 0
        while True:
            j = self.d.find(needle, i)
            if j < 0:
                break
            i = j + 1
            start = self.d.rfind(b"\0", 0, j) + 1
            end = self.d.find(b"\0", j)
            old = bytes(self.d[start:end])
            head = old[:(j - start) + len(b".framework/")]
            leaf = old[(j - start) + len(needle):]
            k = leaf.find(b"/")
            if k < 0:
                continue
            new = head + leaf[k + 1:]
            if len(new) >= len(old):
                continue
            self.d[start:start + len(new)] = new
            self.d[start + len(new)] = 0
            n += 1
        print("  rewrote %d framework dlopen paths (stripped Versions/X/)" % n)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    check_only = "--check" in sys.argv
    shim = "@executable_path/libnodeshim.dylib"
    if "--shim" in sys.argv:
        shim = sys.argv[sys.argv.index("--shim") + 1]

    m = MachO(open(src, "rb").read())

    if "--current" in sys.argv:
        left = m.unshimmed(shim) if m.shim_ordinal(shim) else ["the shim itself"]
        if left:
            print("not current: %s" % ", ".join(left))
            raise SystemExit(1)
        print("current")
        return

    libs = [n for n, _, _, _ in m.dylibs()]
    print("linked dylibs:")
    for i, n in enumerate(libs, 1):
        mark = "" if n in ALLOWED_DYLIBS or n == shim else "   <== NOT KNOWN TO EXIST ON iOS"
        print("  %d: %s%s" % (i, n, mark))
    unknown = [n for n in libs if n not in ALLOWED_DYLIBS and n != shim]
    if unknown:
        raise SystemExit("refusing: unexpected dylib dependency: %s" % ", ".join(unknown))

    _, found = m.find_imports(set(SHIMMED))
    print("shimmed imports present: %d/%d (required %d/%d)"
          % (len(found), len(SHIMMED),
             len([n for n in REQUIRED if n in found]), len(REQUIRED)))
    missing = [n for n in REQUIRED if n not in found]
    if missing:
        raise SystemExit("refusing: required symbols missing: %s" % ", ".join(missing))

    if check_only:
        print("check OK")
        return

    dst = sys.argv[2]
    print("patching:")
    m.set_ios_platform()
    m.rewrite_framework_loads()
    m.add_shim(shim)
    m.fix_framework_paths()
    open(dst, "wb").write(m.d)
    print("wrote %s (%d bytes)" % (dst, len(m.d)))


main()
