#ifndef AT_PATH_H
#define AT_PATH_H

#include <stddef.h>

/* All return 0 on success, -1 with errno set on failure.
 * Directories are created 0700; wrong owner/perms is a hard error. */
int at_runtime_dir(char *out, size_t outsz);
int at_socket_path(char *out, size_t outsz);
int at_sessions_dir(char *out, size_t outsz);

#endif
