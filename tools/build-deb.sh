#!/usr/bin/env bash
# Assemble the .deb from a payload already built by tools/build-payload.sh.
#
#   tools/build-deb.sh [revision]
#
# The version comes from packaging/payload/PAYLOAD.version, which
# build-payload.sh writes, so the two cannot disagree about what was built.
#
# Node itself is NOT packaged. It is ~145MB per version and the published
# repository keeps ten versions for rollback, which would put well over half a
# gigabyte on a GitHub Pages site. We fetch it here only to confirm it is still
# patchable and to record the checksum the postinst re-verifies on the device.
#
# Integrity: nodejs.org publishes SHASUMS256.txt beside each release, and this
# pins the darwin-arm64 line from it at build time. The device then checks the
# tarball it downloads against that pinned value, so a device install trusts
# this build rather than whatever nodejs.org serves it later. (SHASUMS256.txt
# is also PGP-signed by the Node release keys; verifying that here would mean
# shipping and rotating their keyring, and the pin already gives us the
# build-time-to-device integrity that matters.)
#
# Needs: dpkg-deb (apt install dpkg-dev / brew install dpkg), curl, python3.
# The payload must exist already -- run tools/build-payload.sh on macOS first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="https://nodejs.org/dist"
PKG="com.andi.nodejs"

PAYLOAD="$ROOT/packaging/payload"
REVISION="${1:-$(cat "$ROOT/packaging/revision" 2>/dev/null || echo 1)}"
OUT="${OUT:-$ROOT/repo/debs}"
GH_REPO="${GH_REPO:-}"
GH_PAGES="${GH_PAGES:-}"

command -v dpkg-deb >/dev/null || { echo "need dpkg-deb (apt install dpkg-dev)"; exit 1; }
[ -f "$PAYLOAD/libnodeshim.dylib" ] || {
    echo "missing packaging/payload/libnodeshim.dylib -- run tools/build-payload.sh on macOS first"; exit 1; }
[ -f "$PAYLOAD/PAYLOAD.version" ] || {
    echo "missing packaging/payload/PAYLOAD.version -- run tools/build-payload.sh on macOS first"; exit 1; }

read -r VERSION < "$PAYLOAD/PAYLOAD.version"
TARBALL="node-v$VERSION-darwin-arm64.tar.gz"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

echo "==> published checksum for $TARBALL"
curl -fsSL "$DIST/v$VERSION/SHASUMS256.txt" -o "$TMP/SHASUMS256.txt"
TGZ_SHA=$(awk -v f="$TARBALL" '$2 == f {print $1}' "$TMP/SHASUMS256.txt")
[ -n "$TGZ_SHA" ] || { echo "!! $TARBALL not listed in SHASUMS256.txt for v$VERSION"; exit 1; }
echo "    $TGZ_SHA"

echo "==> downloading $TARBALL (~50MB)"
curl -fsSL --retry 3 "$DIST/v$VERSION/$TARBALL" -o "$TMP/node.tar.gz"
GOT=$(sha256 "$TMP/node.tar.gz")
if [ "$GOT" != "$TGZ_SHA" ]; then
    echo "!! tarball does not match the published checksum"
    echo "   SHASUMS256.txt: $TGZ_SHA"
    echo "   downloaded:     $GOT"
    exit 1
fi
echo "    checksum verified"

echo "==> is it still patchable?"
tar xzf "$TMP/node.tar.gz" -C "$TMP" "node-v$VERSION-darwin-arm64/bin/node"
# Refuses if this build links a dylib iOS does not have, dropped a symbol the
# shim must repoint, or uses classic binds instead of chained fixups. Better to
# fail the build than to ship a package whose postinst cannot succeed.
python3 "$PAYLOAD/nodeios_patch.py" "$TMP/node-v$VERSION-darwin-arm64/bin/node" --check

echo "==> staging"
STAGE="$TMP/stage"
LIB="$STAGE/var/jb/usr/local/lib/node-ios"
BIN="$STAGE/var/jb/usr/local/bin"
mkdir -p "$STAGE/DEBIAN" "$LIB" "$BIN"

install -m 755 "$PAYLOAD/libnodeshim.dylib"  "$LIB/libnodeshim.dylib"
install -m 644 "$PAYLOAD/nodeios_patch.py"   "$LIB/nodeios_patch.py"
install -m 644 "$PAYLOAD/shim.c"             "$LIB/shim.c"
install -m 644 "$PAYLOAD/entitlements.plist" "$LIB/entitlements.plist"
install -m 644 "$PAYLOAD/nodeios.sh"         "$LIB/nodeios.sh"
install -m 755 "$PAYLOAD/node-ios-patch"     "$BIN/node-ios-patch"

sed -e "s|@NODE_VERSION@|$VERSION|g" \
    -e "s|@NODE_TARBALL@|$TARBALL|g" \
    -e "s|@NODE_TARBALL_SHA256@|$TGZ_SHA|g" \
    -e "s|@NODE_DIST@|$DIST|g" \
    "$PAYLOAD/version.env.in" > "$LIB/version.env"
chmod 644 "$LIB/version.env"

sed -e "s|@VERSION@|$VERSION-$REVISION|g" \
    -e "s|@NODE_VERSION@|$VERSION|g" \
    -e "s|@REPO@|$GH_REPO|g" -e "s|@PAGES@|$GH_PAGES|g" \
    "$ROOT/packaging/DEBIAN/control.in" > "$STAGE/DEBIAN/control"
[ -n "$GH_REPO"  ] || sed -i.bak '/^Icon:/d'      "$STAGE/DEBIAN/control"
[ -n "$GH_PAGES" ] || sed -i.bak '/^Depiction:/d' "$STAGE/DEBIAN/control"
rm -f "$STAGE/DEBIAN/control.bak"

# Which Node this build packages, where dpkg and apt look for it. Generated
# rather than committed: the upstream version is the only thing that changes
# between releases and it is already known here. The date comes from the
# release's own directory listing, not from `date`, so rebuilding a version
# produces the same bytes instead of tripping the guard below forever.
BUILD_DATE=$(curl -fsSI "$DIST/v$VERSION/$TARBALL" \
             | awk 'tolower($1) == "last-modified:" {sub(/^[^:]*: */, ""); print; exit}')
[ -n "$BUILD_DATE" ] || BUILD_DATE="Thu, 01 Jan 1970 00:00:00 GMT"
DOC="$STAGE/var/jb/usr/share/doc/$PKG"
mkdir -p "$DOC"
cat > "$DOC/changelog" <<CHANGELOG
$PKG ($VERSION-$REVISION) stable; urgency=low

  * Node.js updated to $VERSION.
  * Downloaded from nodejs.org and patched for iOS on install; see
    https://github.com/realAndi/nodejs-for-ios for what the patch does.

 -- andi <tafilajandi@gmail.com>  $BUILD_DATE
CHANGELOG
chmod 644 "$DOC/changelog"

install -m 755 "$ROOT/packaging/DEBIAN/postinst" "$STAGE/DEBIAN/postinst"
install -m 755 "$ROOT/packaging/DEBIAN/prerm"    "$STAGE/DEBIAN/prerm"

mkdir -p "$OUT"
DEB="$OUT/${PKG}_${VERSION}-${REVISION}_iphoneos-arm64.deb"
dpkg-deb -Zxz --root-owner-group --build "$STAGE" "$DEB" >/dev/null
echo "==> $DEB ($(du -h "$DEB" | cut -f1))"

# --- has the payload changed without the revision moving? -------------------
#
# The package version tracks upstream Node, so a change to the shim, the
# patcher, the postinst or the nvm glue that forgets to bump packaging/revision
# republishes an identical version -- and apt, correctly, offers nobody an
# upgrade. The fix reaches no device while every workflow run stays green.
#
# Compared by content, not .deb bytes: an archive carries timestamps and member
# ordering that differ between builds of identical input.
#
# libnodeshim.dylib is excluded because a compiled, ldid-signed binary is never
# byte-identical across build hosts. Its source, shim.c, ships in the package
# and IS compared, so a real change to the shim is still caught. Excluded by
# path rather than by name: `! -name libnodeshim.dylib` would also drop any
# other file that came to share the basename, which is exactly how a sibling
# port lost sight of a second file called `gh`.
#
# version.env is compared and deliberately so -- it records only the upstream
# version, tarball name and published checksum, all of which are fixed for a
# given Node release. Nothing in the package records the shim's own hash, which
# is the other way this guard has been broken before.
payload_digest() {
    local deb="$1" dir
    dir="$(mktemp -d)"
    ( cd "$dir" && ar x "$deb" \
      && mkdir -p x && tar xf data.tar.* -C x 2>/dev/null \
      && tar xf control.tar.* -C x 2>/dev/null )
    ( cd "$dir/x" && find . -type f \
        ! -path './control' ! -path './md5sums' \
        ! -path './var/jb/usr/local/lib/node-ios/libnodeshim.dylib' -print0 | sort -z \
      | xargs -0 shasum -a 256 2>/dev/null ) | shasum -a 256 | cut -d' ' -f1
    rm -rf "$dir"
}

if [ -n "$GH_PAGES" ]; then
    PREV="$TMP/published.deb"
    if curl -fsSL "https://$GH_PAGES/debs/$(basename "$DEB")" -o "$PREV" 2>/dev/null; then
        if [ "$(payload_digest "$PREV")" != "$(payload_digest "$DEB")" ]; then
            echo
            echo "!! $VERSION-$REVISION is already published with different content."
            echo "   Republishing it would change nothing on anyone's device: apt sees"
            echo "   the same version and offers no upgrade."
            echo
            echo "   Bump packaging/revision (currently $REVISION) and rebuild."
            exit 1
        fi
        echo "    matches what is already published at this version"
    else
        echo "    not published yet at this version"
    fi
fi
