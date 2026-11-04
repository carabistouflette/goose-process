/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failure;
static int injected_stop, cancellation;
long __real_syscall(long number, ...);
int __real_kill(pid_t pid, int signal);
pid_t __real_waitpid(pid_t pid, int *status, int options);
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    if (failure == 4) { errno = EPERM; return -1; }
    if (!injected_stop && ((failure == 5 && options == WNOHANG) ||
        (failure == 6 && options == 0) || (failure == 7 && cancellation))) {
        injected_stop = 1;
        *status = (SIGSTOP << 8) | 0x7f;
        return pid;
    }
    return __real_waitpid(pid, status, options);
}
long __wrap_syscall(long number, ...) {
    va_list args; va_start(args, number);
    long result;
    if (number == SYS_pidfd_open) {
        int pid = va_arg(args, int), flags = va_arg(args, int);
        if (failure == 3) { errno = ENOSYS; result = -1; }
        else result = __real_syscall(number, pid, flags);
    } else {
        assert(number == SYS_pidfd_send_signal);
        int pidfd = va_arg(args, int), signal = va_arg(args, int);
        void *info = va_arg(args, void *); int flags = va_arg(args, int);
        if (failure == 1 || failure == 2 || failure == 7) { errno = EPERM; result = -1; }
        else result = __real_syscall(number, pidfd, signal, info, flags);
    }
    va_end(args); return result;
}
int __wrap_kill(pid_t pid, int signal) {
    cancellation = 1;
    if (failure == 2 || failure == 7) { errno = EPERM; return -1; }
    return __real_kill(pid, signal);
}
int main(void) {
    uint8_t packed[] = "/bin/sleep\0" "10\0";
    uint8_t quick[] = "/bin/true\0";
    for (failure = 1; failure <= 7; ++failure) {
        injected_stop = 0; cancellation = 0;
        int64_t meta[6];
        gp_input command = failure == 5 ? (gp_input){quick, sizeof quick - 1} :
                                        (gp_input){packed, sizeof packed - 1};
        int result = (int)gp_run(command, (gp_input){NULL, 0},
            20, 1, (gp_output){NULL, 0}, (gp_output){NULL, 0}, (gp_metadata){meta, 6});
        if (failure == 1 || failure == 6)
            assert(result == 4 && meta[2] == SIGKILL && meta[5] == 0);
        if (failure == 5)
            assert(result == 0 && meta[1] == 0 && meta[5] == 0 && injected_stop);
        if (failure == 6 || failure == 7) assert(injected_stop);
        if (failure == 2 || failure == 4 || failure == 7) {
            assert(result == 3 && meta[0] == EPERM && meta[5] > 0);
            pid_t pid = (pid_t)meta[5];
            assert(__real_kill(pid, SIGKILL) == 0);
            int status; assert(__real_waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
        }
        if (failure == 3) assert(result == 3 && meta[0] == ENOSYS && meta[2] == SIGKILL && meta[5] == 0);
    }
    puts("cancellation failure tests passed");
    return 0;
}
