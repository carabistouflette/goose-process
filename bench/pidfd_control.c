/* SPDX-License-Identifier: Apache-2.0 */
/* Plain-libc instrumentation control, never linked into the library. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) { for (;;) pause(); }
    int fd = (int)syscall(SYS_pidfd_open, child, 0); assert(fd >= 0);
    errno = 0;
    int result = (int)syscall(SYS_pidfd_send_signal, fd, SIGKILL, NULL, 0);
    int error = errno;
    printf("pidfd_send_signal result=%d errno=%d\n", result, error);
    /* If the profiler cannot emulate the syscall, clean this exclusively owned
       unreaped fixture by its reserved PID. This is not a library workaround. */
    if (result < 0) assert(kill(child, SIGKILL) == 0);
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(close(fd) == 0);
    puts("owned control child reaped");
    return result < 0 ? 77 : 0;
}
