# com.andi.nodejs: put the commands `npm install -g` installs on PATH.
#
# The package sets npm's global prefix to ~/.npm-global, so each user's global
# packages live in their own home: no sudo, since the shared bin directory is
# root's, and nothing lost when an upgrade replaces Node's distribution.
#
# Appended, not prepended. An `npm install -g npm` lands in there too, and its
# npm is a #! script that node and Pi cannot spawn; the packaged npm launcher
# earlier on PATH has to stay the one that runs.
#
# Sourced by /var/jb/etc/profile and /var/jb/etc/zprofile, so login shells
# only. A one-off `ssh host cmd` does not read it.
if [ -n "${HOME-}" ]; then
    case ":${PATH-}:" in
        *":$HOME/.npm-global/bin:"*) ;;
        *) PATH="${PATH:+$PATH:}$HOME/.npm-global/bin"; export PATH ;;
    esac
fi
