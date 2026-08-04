/* path.c — runtime directory + socket path resolution with strict perms.
 *
 * Policy: $XDG_RUNTIME_DIR/agent-terminal/ if XDG_RUNTIME_DIR is set (Linux
 * convention), else ~/.agent-terminal/run/. The directory is created 0700 and
 * we HARD-FAIL if it exists with the wrong owner or with group/other bits set
 * — never silently chmod, because that would paper over a symlink/squat. */
#include "path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xutil.h"

static int ensure_private_dir(const char *dir) {
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(dir, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (st.st_uid != getuid()) { errno = EPERM; return -1; }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) { errno = EPERM; return -1; }
    return 0;
}

int at_runtime_dir(char *out, size_t outsz) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg == '/') {
        if ((size_t)snprintf(out, outsz, "%s/agent-terminal", xdg) >= outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return ensure_private_dir(out);
    }
    const char *home = getenv("HOME");
    if (!home || *home != '/') { errno = ENOENT; return -1; }
    char base[512];
    if ((size_t)snprintf(base, sizeof base, "%s/.agent-terminal", home) >= sizeof base) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_private_dir(base) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/run", base) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return ensure_private_dir(out);
}

int at_socket_path(char *out, size_t outsz) {
    char dir[512];
    if (at_runtime_dir(dir, sizeof dir) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/default.sock", dir) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int at_sessions_dir(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (!home || *home != '/') { errno = ENOENT; return -1; }
    char base[512];
    if ((size_t)snprintf(base, sizeof base, "%s/.agent-terminal", home) >= sizeof base) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_private_dir(base) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/sessions", base) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return ensure_private_dir(out);
}
