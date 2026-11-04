/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failure;
static int injected_stop, cancellation;
static pid_t observed_pid;
static int wait_denials, signal_faults, signal_attempts;
long __real_syscall(long number, ...);
int __real_kill(pid_t pid, int signal);
pid_t __real_waitpid(pid_t pid, int *status, int options);
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    if (pid == observed_pid && wait_denials) {
        --wait_denials; errno = EPERM; return -1;
    }
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
        if (failure == 3 || failure == 8) { errno = ENOSYS; result = -1; }
        else result = __real_syscall(number, pid, flags);
    } else {
        assert(number == SYS_pidfd_send_signal);
        int pidfd = va_arg(args, int), signal = va_arg(args, int);
        void *info = va_arg(args, void *); int flags = va_arg(args, int);
        if (signal_faults) {
            ++signal_attempts; --signal_faults;
            errno = signal_faults ? EINTR : EPERM; result = -1;
        } else if (failure == 1 || failure == 2 || failure == 7) { errno = EPERM; result = -1; }
        else result = __real_syscall(number, pidfd, signal, info, flags);
    }
    va_end(args); return result;
}
int __wrap_kill(pid_t pid, int signal) {
    cancellation = 1;
    if (failure == 2 || failure == 7 || failure == 8) { errno = EPERM; return -1; }
    return __real_kill(pid, signal);
}
static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    int count = 0;
    while (readdir(directory)) ++count;
    assert(closedir(directory) == 0);
    return count;
}
static gp_input empty(void) { return (gp_input){NULL, 0}; }
static gp_metadata words(int64_t *data, int64_t length) { return (gp_metadata){data, length}; }
static int start(gp_metadata state, gp_metadata meta, bool quick) {
    uint8_t sleeping[] = "/bin/sleep\0" "60\0", done[] = "/bin/true\0";
    int64_t config[11] = {5000, 1, 1, 0, -1, -1, -1, 0, 0, 0, 0};
    gp_input command = quick ? (gp_input){done, sizeof done - 1} :
                              (gp_input){sleeping, sizeof sleeping - 1};
    return (int)gp_start(command, empty(), empty(), empty(), 0, words(config, 11), state, meta);
}
/* Operation errors preserve the active owner; only terminal cleanup transfers. */
static void recoverable_operations(void) {
    int before = descriptors();
    int64_t storage[24] = {0}, values[6], done = -1;
    gp_metadata state = words(storage, 24), meta = words(values, 6);
    failure = 0;
    assert(start(state, meta, false) == 0);
    observed_pid = (pid_t)gp_id(state); assert(observed_pid > 0);
    wait_denials = 1;
    assert(gp_try_wait(state, meta, &done) == 3 && values[0] == EPERM && done == 0);
    assert(wait_denials == 0 && values[5] == 0);
    assert(gp_id(state) == observed_pid && !gp_can_start(state));
    assert(gp_try_wait(state, meta, &done) == 0 && values[0] == 0 && done == 0);
    signal_faults = 2; signal_attempts = 0;
    assert(gp_signal(state, SIGTERM, meta) == 3 && values[0] == EPERM);
    assert(signal_faults == 0 && signal_attempts == 2 && values[5] == 0);
    assert(gp_id(state) == observed_pid && !gp_can_start(state));
    assert(gp_signal(state, 0, meta) == 0 && values[0] == 0);
    assert(gp_signal(state, SIGTERM, meta) == 0);
    assert(gp_wait(state, empty(), (gp_output){NULL, 0}, (gp_output){NULL, 0},
        0, 0, meta, &done) == 0);
    assert(done && values[0] == 0 && values[1] == -1 && values[2] == SIGTERM && values[5] == 0);
    assert(gp_id(state) == -1 && gp_can_start(state));
    assert(gp_close(state, meta) == 0 && values[5] == 0);
    int status;
    assert(waitpid(observed_pid, &status, WNOHANG) == -1 && errno == ECHILD);
    observed_pid = 0;
    assert(descriptors() == before);
}
static void failed_start_transfer(void) {
    int before = descriptors();
    int64_t storage[24] = {0}, values[6];
    gp_metadata state = words(storage, 24), meta = words(values, 6);
    failure = 8;
    assert(start(state, meta, false) == 3 && values[0] == EPERM && values[5] > 0);
    pid_t transferred = (pid_t)values[5];
    assert(gp_id(state) == -1 && gp_can_start(state));
    assert(gp_close(state, meta) == 3 && values[5] == 0);
    assert(gp_close(state, meta) == 3 && values[5] == 0);
    /* Child is closed. This separate recovery owner holds the unreaped PID. */
    assert(__real_kill(transferred, SIGKILL) == 0);
    int status; pid_t waited;
    do { waited = __real_waitpid(transferred, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == transferred && WIFSIGNALED(status));
    failure = 0;
    assert(start(state, meta, true) == 0);
    int64_t done;
    assert(gp_wait(state, empty(), (gp_output){NULL, 0}, (gp_output){NULL, 0},
        0, 0, meta, &done) == 0 && done && values[1] == 0 && values[5] == 0);
    assert(descriptors() == before);
}
static void denied_after_exit(void) {
    int before = descriptors();
    int64_t storage[24] = {0}, values[6];
    gp_metadata state = words(storage, 24), meta = words(values, 6);
    failure = 0; assert(start(state, meta, true) == 0);
    pid_t pid = (pid_t)gp_id(state); assert(pid > 0);
    siginfo_t info = {0}; int result;
    /* Establish natural exit without reaping or releasing its PID reservation. */
    do { result = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    assert(result == 0 && info.si_pid == pid && info.si_code == CLD_EXITED);
    failure = 2;
    assert(gp_close(state, meta) == 3 && values[0] == EPERM);
    assert(values[1] == 0 && values[2] == 0 && values[5] == 0 && gp_id(state) == -1);
    failure = 0;
    int status;
    assert(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(descriptors() == before);
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
    recoverable_operations(); failed_start_transfer(); denied_after_exit();
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("cancellation and recoverable operation failure tests passed");
    return 0;
}
