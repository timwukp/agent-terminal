#ifndef AT_PATH_H
#define AT_PATH_H

#include <stdbool.h>
#include <stddef.h>

/* All return 0 on success, -1 with errno set on failure.
 * Directories are created 0700; wrong owner/perms is a hard error. */
int at_runtime_dir(char *out, size_t outsz);
int at_socket_path(char *out, size_t outsz);
int at_sessions_dir(char *out, size_t outsz);

/* True if `name` is safe to interpolate into a path as a single component.
 * A session name becomes a directory under the sessions dir, so it must not
 * contain '/', be "." or "..", or start with '.'. Callers that build paths
 * from a name MUST gate on this. */
bool at_valid_session_name(const char *name);

#endif
