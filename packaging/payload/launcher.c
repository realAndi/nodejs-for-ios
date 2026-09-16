/*  node-ios launcher -- a signed Mach-O standing in for npm, npx and corepack.
 *
 *  In Node's own distribution those three are symlinks to JavaScript files
 *  that begin `#!/usr/bin/env node`. On a rootless jailbreak a #! line is only
 *  honoured when a bootstrap binary does the exec -- zsh, bash, python3 --
 *  because the jailbreak handles it for them in userland. Anything else gets
 *  EPERM from the kernel, or ENOENT when the interpreter is /usr/bin/env, which
 *  exists only under /var/jb. "Anything else" includes this port's own node
 *  and the Bun runtime behind Pi, so `spawn("npm")` failed from both, and
 *  `pi install npm:...` with it, while `npm` typed at a shell worked.
 *
 *  A signed Mach-O execs from anywhere. So each command is a copy of this
 *  program, installed as <lib>/bin/<name>, and it runs
 *
 *      <lib>/dist/bin/node  <lib>/dist/bin/<name>, resolved  args...
 *
 *  which is what the kernel would have done with the #! line. Everything is
 *  found from the launcher's own real path, not argv[0] -- a caller may set
 *  argv[0] to anything -- so the file's name alone picks the command, and the
 *  script behind it comes from Node's own symlink rather than a table here that
 *  would have to track where each Node release keeps npm.
 */

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    char exe[PATH_MAX], lib[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || realpath(exe, lib) == NULL) {
        fprintf(stderr, "node-ios: cannot resolve the launcher's own path: %s\n",
                strerror(errno));
        return 127;
    }

    /* lib is <lib>/bin/<name>; split it into <lib> and <name>. */
    char name[NAME_MAX + 1];
    char *slash = strrchr(lib, '/');
    if (slash == NULL || strlcpy(name, slash + 1, sizeof name) >= sizeof name) {
        fprintf(stderr, "node-ios: unexpected launcher path %s\n", lib);
        return 127;
    }
    *slash = '\0';
    slash = strrchr(lib, '/');
    if (slash == NULL || strcmp(slash + 1, "bin") != 0) {
        fprintf(stderr, "node-ios: launcher must live in <lib>/bin, not %s\n", lib);
        return 127;
    }
    *slash = '\0';

    char node[PATH_MAX], link[PATH_MAX], script[PATH_MAX];
    if ((size_t)snprintf(node, sizeof node, "%s/dist/bin/node", lib) >= sizeof node ||
        (size_t)snprintf(link, sizeof link, "%s/dist/bin/%s", lib, name) >= sizeof link) {
        fprintf(stderr, "node-ios: path too long under %s\n", lib);
        return 127;
    }
    if (access(node, X_OK) != 0) {
        fprintf(stderr, "node-ios: %s is missing; Node.js did not finish installing. "
                        "Reinstall com.andi.nodejs.\n", node);
        return 127;
    }
    if (realpath(link, script) == NULL) {
        fprintf(stderr, "node-ios: this Node.js has no %s (%s: %s)\n",
                name, link, strerror(errno));
        return 127;
    }

    /* node, script, then everything after our own argv[0]. */
    char **args = calloc((size_t)argc + 2, sizeof *args);
    if (args == NULL) {
        perror("node-ios: calloc");
        return 127;
    }
    args[0] = node;
    args[1] = script;
    for (int i = 1; i < argc; i++) args[i + 1] = argv[i];

    execv(node, args);
    fprintf(stderr, "node-ios: cannot exec %s: %s\n", node, strerror(errno));
    return 126;
}
