# node-ios: make nvm work on a rootless jailbreak.
#
#   . /var/jb/usr/local/lib/node-ios/nodeios.sh
#
# Source it from ~/.zshrc (or ~/.bash_profile) after installing nvm.

# NVM_DIR has to live under the bootstrap prefix. nvm's own default is ~/.nvm,
# which on a device is /var/mobile/.nvm -- and the sandbox refuses to mmap() a
# dylib out of that tree, so libnodeshim.dylib cannot load and every node it
# installs dies in dyld before reaching main. Override it rather than let people
# discover that as "node is broken".
export NVM_DIR="${NVM_DIR:-/var/jb/var/mobile/.nvm}"
[ -s "$NVM_DIR/nvm.sh" ] && . "$NVM_DIR/nvm.sh"

if command -v nvm >/dev/null 2>&1; then
  # iOS `uname -m` answers the device model ("iPhone16,1") and there is no
  # sysctl(8), so nvm derives a darwin-iPhone16,1 target, 404s on every binary
  # download, and falls back to a source build that cannot work here. Every iOS
  # device new enough to run this is arm64.
  nvm_get_arch() { nvm_echo "arm64"; }

  # nvm installs Apple's macOS build, which needs patching before it will run.
  # Keep the original under another name and wrap it, so `nvm install` stays the
  # command people already know.
  eval "nvm_upstream() $(declare -f nvm | tail -n +2)"
  nvm() {
    nvm_upstream "$@"
    local rc=$?
    case "$1" in
      install|i)
        if [ $rc -eq 0 ]; then
          # stdout only: a patch that fails has to say so, or `nvm use` hands
          # people a macOS binary that dies in dyld with no hint why.
          command -v node-ios-patch >/dev/null 2>&1 \
            && node-ios-patch --all >/dev/null
        fi
        ;;
    esac
    return $rc
  }
fi
