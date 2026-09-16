#!/usr/bin/env bash
# Build everything the package will contain, into packaging/payload/.
#
#   tools/build-payload.sh <version>
#
# macOS only: libnodeshim.dylib and node-ios-launcher are compiled against
# Xcode's iPhoneOS SDK, which is the one thing in this port that cannot be done
# on Linux. Everything else --
# fetching Node, checking it is patchable, assembling the .deb -- is Linux work
# and lives in build-deb.sh.
#
# The shim does not depend on the Node version. It is written against libSystem
# and V8's use of pthread_jit_write_protect_np, neither of which moves between
# Node releases, so <version> is recorded rather than compiled in. The version
# that matters is checked in build-deb.sh, which runs nodeios_patch.py --check
# against the real tarball and refuses to package one the shim cannot serve.
#
# This script checks for its toolchain and exits; it never installs anything.
# The caller declares what it needs (see .github/workflows/publish.yml), so a
# laptop run fails the same way a runner does instead of silently differing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PAYLOAD="$ROOT/packaging/payload"
VERSION="${1:?usage: build-payload.sh <version>}"
VERSION="${VERSION#v}"

CI_TOOLS="${CI_TOOLS:-$("$ROOT/tools/ci.sh")}"

[ "$(uname -s)" = "Darwin" ] || { echo "build-payload.sh needs macOS (iPhoneOS SDK)" >&2; exit 1; }
command -v xcrun >/dev/null || { echo "need Xcode command line tools" >&2; exit 1; }
# iOS refuses to load an unsigned dylib ("mapped file has no cdhash"), and the
# runners have no ldid, so the caller installs it via brew-packages.
command -v ldid  >/dev/null || { echo "need ldid (brew install ldid)" >&2; exit 1; }

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "==> iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version) at $SDK"

echo "==> compiling libnodeshim.dylib for iphoneos-arm64"
# -miphoneos-version-min=15.0 matches what nodeios_patch.py stamps into
# LC_BUILD_VERSION. Below 15.0 is untested and the port claims firmware >= 15.0.
xcrun --sdk iphoneos clang \
    -arch arm64 -miphoneos-version-min=15.0 \
    -dynamiclib -O2 -Wall -Wextra \
    -install_name @executable_path/libnodeshim.dylib \
    -o "$PAYLOAD/libnodeshim.dylib" "$PAYLOAD/shim.c"

ldid -S "$PAYLOAD/libnodeshim.dylib"

# npm, npx and corepack are `#!/usr/bin/env node` scripts, and on a rootless
# jailbreak only a bootstrap binary can exec one -- node itself and Pi's runtime
# get EPERM or ENOENT. The package points those commands at copies of this
# instead, a signed Mach-O that execs node with the script. See launcher.c.
echo "==> compiling node-ios-launcher for iphoneos-arm64"
xcrun --sdk iphoneos clang \
    -arch arm64 -miphoneos-version-min=15.0 \
    -O2 -Wall -Wextra \
    -o "$PAYLOAD/node-ios-launcher" "$PAYLOAD/launcher.c"

ldid -S "$PAYLOAD/node-ios-launcher"

# build-shim's sibling port shipped an unsigned dylib once because ldid was
# missing and the build only warned. Assert it here, as a hard failure: an
# unsigned shim or launcher produces a package that cannot possibly work, and
# finding that out on a device is far more expensive than finding it out now.
if command -v codesign >/dev/null 2>&1; then
    for f in libnodeshim.dylib node-ios-launcher; do
        # Captured, not piped into `grep -q`: grep exits at the first match and
        # closes the pipe, codesign takes SIGPIPE, and `set -o pipefail` then
        # fails the check that just succeeded. codesign also prints "no
        # signature" for an ldid ad-hoc signature -- it has no CMS blob -- so
        # CodeDirectory is the thing to look for.
        sig="$(codesign -dv "$PAYLOAD/$f" 2>&1 || true)"
        case "$sig" in
            *CodeDirectory*) ;;
            *) echo "$f is not signed -- iOS would refuse to load it" >&2; exit 1 ;;
        esac
    done
fi

echo "==> checking the shim and launcher really are iOS Mach-Os"
python3 "$CI_TOOLS/check-macho.py" "$PAYLOAD/libnodeshim.dylib"
python3 "$CI_TOOLS/check-macho.py" "$PAYLOAD/node-ios-launcher"

# Every symbol the shim exports must be one the patcher knows how to repoint,
# and vice versa for the required ones. A shim that stopped exporting
# pthread_jit_write_protect_np would otherwise build cleanly and fail on device.
python3 - "$PAYLOAD/libnodeshim.dylib" "$PAYLOAD/nodeios_patch.py" <<'PY'
import re, subprocess, sys
dylib, patcher = sys.argv[1], sys.argv[2]
out = subprocess.run(["nm", "-gU", dylib], capture_output=True, text=True).stdout
exported = {l.split()[-1] for l in out.splitlines() if l.strip()}
src = open(patcher).read()
required = re.search(r"REQUIRED = \[(.*?)\]", src, re.S).group(1)
required = re.findall(r'"([^"]+)"', required)
missing = [s for s in required if s not in exported]
if missing:
    sys.exit("shim does not export required symbol(s): %s" % ", ".join(missing))
print("    shim exports all %d required symbols" % len(required))
PY

echo "$VERSION" > "$PAYLOAD/PAYLOAD.version"
echo "==> payload ready for Node $VERSION"
ls -la "$PAYLOAD"
