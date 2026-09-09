#!/usr/bin/env bash
# Where the shared publishing tools live. Echoes that path and nothing else.
#
#   CI_TOOLS="$(tools/ci.sh)"
#
# check-macho.py, clangwrap.sh, make-repo.py and fetch-published.sh are not in
# this repository any more: they are identical in every port, so they live in
# realAndi/ios-port-ci and a fix to one of them reaches all the ports at once.
#
# On a runner the workflow checks that repository out at .ci/ and sets CI_TOOLS
# itself, so this script never runs there. Locally it clones the same pinned
# ref, so build-payload.sh and build-deb.sh behave the same on a laptop as in
# CI rather than only ever being exercised by the workflow.
set -euo pipefail

CI_REPO="https://github.com/realAndi/ios-port-ci"
# Pinned. Moving to a new major means the contract changed and this repo's
# scripts changed with it -- not something a push to the CI repo's main should
# be able to do to a build here.
CI_REF="v1"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/.ci"

if [ ! -d "$DIR/tools" ]; then
    echo "==> cloning $CI_REPO@$CI_REF into .ci/" >&2
    rm -rf "$DIR"
    git -c advice.detachedHead=false clone --quiet --depth 1 \
        --branch "$CI_REF" "$CI_REPO" "$DIR" >&2
fi

echo "$DIR/tools"
