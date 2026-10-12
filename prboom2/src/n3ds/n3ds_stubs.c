//
// [3DS] What the 3DS's C library lacks.
//

#include <errno.h>
#include <sys/types.h>

// i_capture.c (video capture through external programs): no processes.
int pipe(int fds[2]) { errno = ENOSYS; return -1; }
int execl(const char *path, const char *arg, ...) { errno = ENOSYS; return -1; }
pid_t waitpid(pid_t pid, int *status, int options) { errno = ENOSYS; return -1; }
