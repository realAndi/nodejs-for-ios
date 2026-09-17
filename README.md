# Node.js for jailbroken iOS

Current Node.js — with npm, npx and working nvm — on a rootless jailbreak.
Published as a Sileo/APT package at
[reallyitsandi.com/repo/](https://reallyitsandi.com/repo/).

```
Package: com.andi.nodejs
Repo:    https://reallyitsandi.com/repo/
Tested:  iPhone 15 Pro, iOS 17.3, Dopamine (rootless, /var/jb)
```

## Why this is not a cross-compile

The only prior art for Node on iOS cross-compiles from source: nodejs-mobile is
frozen at 18.20.4 and builds a framework rather than a CLI, and the one public
Node 22 iOS build carries a substantial V8 patch series and a two-hour CI job.

This port does something much cheaper. Apple's own `darwin-arm64` build of Node
imports 725 symbols, and iOS 17.3 is missing exactly **three** of them:

| symbol | why iOS lacks it | what the shim does |
|---|---|---|
| `pthread_jit_write_protect_np` | macOS-only; the SDK marks it `__API_UNAVAILABLE(ios)` | reimplements the W^X flip with `mprotect` |
| `SecTrustSettingsCopyTrustSettings` | macOS-only keychain trust API | returns `errSecNoTrustSettings`, so Node keeps its bundled Mozilla roots |
| `syslog$DARWIN_EXTSN` | iOS exports plain `syslog`, not the alias | forwards to `vsyslog` |

So the port is a 68 KB shim plus a Mach-O rewrite, and it works for *any* Node
release with the same shape. No V8 patches, no source build, no ICU decisions.
The same approach the sibling [CCForiOS](https://github.com/realAndi/CCForiOS)
port uses for Bun, and much cheaper here because Node's import surface is small
and its licence lets it be redistributed.

The rewrite itself (`nodeios_patch.py`, pure stdlib so it runs on the phone):

1. `LC_BUILD_VERSION` platform → iOS, minos 15.0.
2. The CoreFoundation and Security `LC_LOAD_DYLIB` paths, which on macOS point
   into `.framework/Versions/A/`, rewritten to the flat iOS layout. Both
   frameworks exist on iOS; only the path shape differs. The replacement is
   shorter, so it goes in place and the load command keeps its size.
3. An `LC_LOAD_DYLIB` for the shim appended into the header slack (Node has
   ~13.8 KB free), and the shimmed imports repointed to its ordinal inside
   `LC_DYLD_CHAINED_FIXUPS`.
4. Any leftover `.framework/Versions/X/` string in the data rewritten.

It refuses to patch a binary that links a dylib iOS does not have, or that has
dropped a symbol the shim must repoint, so a build that changed shape fails
loudly instead of producing something that dies on device.

## How the JIT works

Established by running a probe on the device rather than assuming:

| strategy | result on iOS 17.3 / A17 Pro |
|---|---|
| `mmap` with `PROT_READ\|WRITE\|EXEC` | SIGBUS on execute |
| `mmap` with `MAP_JIT` | `EINVAL` |
| `mmap` RW → write → `mprotect` RX → execute | **works** |

`pthread_jit_write_protect_np` is absent, and it is per thread: the thread
writing code sees its pages read-write while every other thread keeps executing
them. iOS can only change protection for the whole process. So the shim tracks
every 16 KB page of V8's code ranges as RW or RX and lets faults move them:

| event | what the shim does |
|---|---|
| `mmap` with `MAP_JIT` | strips the flag and records the range |
| `mprotect` or `munmap` on a code range | passes it on and updates the record; RWX is granted as RW |
| `pthread_jit_write_protect_np(0)` | marks this thread as writing; changes no protection |
| write fault by a writing thread | makes that one page RW and counts the thread as its writer |
| execute fault | waits until no thread is still writing the page, makes it RX |
| `pthread_jit_write_protect_np(1)` | drops this thread's writer counts |

After a repair the faulting instruction runs again. The faults arrive as
SIGBUS or SIGSEGV, which V8's WebAssembly trap handler and Node also claim, so
the shim installs its handler before `main` and interposes `sigaction` and
`signal` for those two signals. Their handlers are recorded and chained to for
every fault that is not a code-page repair, and a crash signal still ends the
process with the same status.

The first version of the shim `mprotect`ed every code range on every call
instead. That was process-wide, so a worker isolate compiling pulled the main
isolate's code out from under it. Measured on an iPhone 15 Pro, iOS 17.3, with
Node 26.9.0:

| test | old flip | page faults |
|---|---|---|
| two CPU-heavy workers | 0 of 5 survived | 20 of 20 |
| four workers with polymorphic code, busy main thread | 0 of 3 | 25 of 25 |

Ten runs in each row had V8's background threads on. It is faster too,
because a write no longer re-protects 256 MB per isolate twice:

| benchmark | old flip | page faults |
|---|---|---|
| JSON round trips | 200–329 ms | 156–179 ms |
| property access on mixed shapes | 115–117 ms | 62–65 ms |

The shim still forces V8 `--single-threaded`, conservatively, though the
worker tests pass without it. The flag is rejected in `NODE_OPTIONS`, which
rules out the obvious way to reach child processes. Node exports
`v8::V8::SetFlagsFromString`, though, so the shim sets the flag from a library
constructor — before `main`, long before `V8::Initialize`. That covers every
route into the binary: the command line, a `#!/usr/bin/env node` shebang, and
anything npm spawns through `process.execPath`. `NODEIOS_V8_FLAGS` overrides
it; the empty string opts out.

A Node patched before this change links the shim but does not route
`sigaction`, `mprotect` or `munmap` through it, and would crash beside the new
shim. `node-ios-patch` therefore asks the patcher whether a binary is current
rather than whether it links the shim, and patches it again if not. The patcher
reuses the existing load command, so the result is byte-identical to patching
from scratch.

## The other iOS problems, and what they cost

- **`/bin/sh` does not exist** on a rootless jailbreak, and Node hard-codes it
  for `shell: true`. Every `execSync` and every npm lifecycle script failed with
  ENOENT. The shim interposes `posix_spawn`, `posix_spawnp`, `execve` and
  `execvp` and retries an absolute system path under `/var/jb` — but only when
  the original really is missing, so nothing is remapped out from under a device
  that does have the file.
- **The sandbox will not `mmap` a dylib out of some trees.** A staged binary
  under `/var/mobile` dies in dyld with `file system sandbox blocked mmap()`
  before it reaches `main`, and a binary under `/var/jb/tmp` is SIGKILLed with
  no crash report at all. The postinst stages inside its own install directory
  for both reasons, and because same-filesystem makes the final move atomic.
- **Nothing but a bootstrap binary can run a `#!` script.** zsh, bash and
  python3 exec one fine, because the jailbreak handles the `#!` line for them.
  Node and Pi's Bun runtime get EPERM, or ENOENT for `#!/usr/bin/env`, which
  exists only under `/var/jb`. `npm` and `npx` are exactly such scripts, so
  typing `npm` worked while `spawn("npm")` from Node and `pi install npm:...`
  both failed. The package therefore links `npm`, `npx` and `corepack` to a
  tiny signed Mach-O launcher (`launcher.c`) that execs node with the script.
  The shim covers everything else: when a spawn or exec fails that way on an
  executable `#!` script, it runs the interpreter itself, so Node can spawn the
  commands `npm install -g` creates as well.
- **`uname -m` answers the device model** (`iPhone16,1`), not the architecture,
  and there is no `sysctl(8)`. Stock nvm therefore builds a
  `darwin-iPhone16,1` download URL, 404s, and falls back to a source compile
  that cannot work. `nodeios.sh` overrides `nvm_get_arch`.

## Global packages

`npm install -g` installs into `~/.npm-global`, per user and without sudo.
The package sets that as npm's builtin config and adds
`/var/jb/etc/profile.d/node-ios.sh`, which puts `~/.npm-global/bin` on PATH in
login shells. Open a new shell after installing the package.

Two consequences of the layout, both deliberate:

- Global packages survive package upgrades. Before revision 4 they were written
  into Node's own distribution directory, off PATH, and each upgrade deleted
  them. Anything installed that way needs installing again.
- `~/.npm-global/bin` is appended to PATH, not prepended, so
  `npm install -g npm` does not replace the packaged npm. Its npm is a `#!`
  script that Node and Pi could not spawn.

## Using nvm

```sh
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.7/install.sh | bash
echo '. /var/jb/usr/local/lib/node-ios/nodeios.sh' >> ~/.zshrc
exec zsh
nvm install 24
```

`nodeios.sh` sources nvm, fixes the architecture detection, and wraps `nvm` so
that anything `nvm install` brings down is patched and signed before you use it.
`node-ios-patch --all` does the same by hand, and is idempotent.

## Known limits

- **Node 24 or newer.** Node 23 and earlier are built with a macOS deployment
  target of 11.0, so the linker emits classic `LC_DYLD_INFO_ONLY` bind opcodes
  instead of chained fixups. Repointing those means rewriting the bind opcode
  stream, which this port does not do. `tools/resolve-version.sh` refuses them.
- **Creating many workers in quick succession can run out of address space.**
  With the JIT on, each isolate reserves a 256 MB code range plus V8's tables,
  and iOS gives a process far less address space than macOS. Starting 40
  workers four at a time failed with `Fatal process out of memory` in 6 of 22
  runs; the same test with `NODEIOS_V8_FLAGS=--jitless` passed every
  time. Workers that live for a while are unaffected.
- **Prebuilt native addons do not load.** A `.node` from npm is a macOS
  build, and dyld refuses it as the wrong platform. `node-ios-patch` cannot
  convert one yet: it patches node itself, and refuses a file without V8's JIT
  imports.

## What was measured on device

iPhone 15 Pro, iOS 17.3, Node 26.8.1, in the default (JIT) mode:

| check | result |
|---|---|
| TurboFan hot loop, 20 consecutive runs | 0 failures |
| WebAssembly instantiate + call | pass |
| `fetch()` over HTTPS to the npm registry | 200 |
| `node:sqlite` | pass |
| `child_process.execSync` | pass |
| `npm install lodash chalk` | 2 packages, 816 ms |
| `npx cowsay` | pass |
| `nvm install 24`, patch, `nvm use` | pass |

## Layout

Three scripts, per the [ios-port-ci contract](https://github.com/realAndi/ios-port-ci):

| path | runs on | does |
|---|---|---|
| `tools/resolve-version.sh` | Linux | echoes the Node version to build, refusing anything below 24 |
| `tools/build-payload.sh` | macOS | compiles and signs `libnodeshim.dylib` and the npm launcher against the iPhoneOS SDK |
| `tools/build-deb.sh` | Linux | fetches Node, verifies its checksum, proves it is patchable, assembles the `.deb` |

Node is not in the package. It is ~145 MB per version and the published
repository keeps ten for rollback; the package is ~20 KB and the postinst
downloads, verifies, patches and signs on the device.

## Building locally

```sh
V=$(tools/resolve-version.sh)
tools/build-payload.sh "$V"     # macOS: Xcode + ldid
tools/build-deb.sh              # dpkg-deb, curl, python3
```
