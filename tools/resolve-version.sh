#!/usr/bin/env bash
# Which Node.js release should we package?
#
#   tools/resolve-version.sh          the newest release nodejs.org lists
#   tools/resolve-version.sh 26.8.1   that version, validated
#
# Echoes the bare version on stdout and NOTHING else -- CI captures it. No
# leading "v": that belongs in nodejs.org URLs, not in a Debian version field.
# Progress and errors go to stderr.
#
# Runs on Linux, before the macOS job exists. See CONTRACT.md in ios-port-ci.
set -euo pipefail

DIST="https://nodejs.org/dist"

# Node 23 and earlier are built with a macOS deployment target of 11.0, which
# makes the linker emit classic LC_DYLD_INFO_ONLY bind opcodes. The patcher
# repoints imports inside LC_DYLD_CHAINED_FIXUPS and has no opcode rewriter, so
# those builds cannot be shimmed. 24.0.0 is where nodejs.org's darwin-arm64
# builds move to minos 13.5 and chained fixups. Refuse earlier versions here,
# loudly, rather than three minutes later inside build-deb.sh.
MIN_MAJOR=24

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "$DIST/index.json" | python3 -c '
import json, sys
rs = json.load(sys.stdin)
def key(r):
    return tuple(int(p) for p in r["version"].lstrip("v").split("."))
# index.json is newest-first already, but sort rather than trust the ordering.
print(max(rs, key=key)["version"].lstrip("v"))
')
    echo "==> newest release on nodejs.org is $VERSION" >&2
fi

VERSION="${VERSION#v}"

case "$VERSION" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "not a Node version: $VERSION (want e.g. 26.8.1)" >&2; exit 1 ;;
esac

MAJOR="${VERSION%%.*}"
if [ "$MAJOR" -lt "$MIN_MAJOR" ]; then
    echo "Node $VERSION is too old to patch: its darwin-arm64 build uses classic" >&2
    echo "LC_DYLD_INFO_ONLY binds, not chained fixups. Need >= $MIN_MAJOR.0.0." >&2
    exit 1
fi

# A version that does not exist would otherwise fail minutes later, in
# build-deb.sh, after paying for a checkout and a macOS shim build.
curl -fsSL --head "$DIST/v$VERSION/SHASUMS256.txt" >/dev/null 2>&1 \
    || { echo "nodejs.org has no release v$VERSION" >&2; exit 1; }

echo "$VERSION"
