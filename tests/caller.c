/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Application-owned handlers and recovery, not global library hooks. This
   reference uses the private native ABI only to exercise the caller boundary. */
static volatile sig_atomic_t pending_signal, child_signal_count;
static bool deny_cancel;
static int denied_pidfd, denied_kill, denied_waits;
static pid_t owned_pid;
static char executable[PATH_MAX];

long __real_syscall(long number, ...);
int __real_kill(pid_t pid, int number);
pid_t __real_waitpid(pid_t pid, int *status, int options);
long __wrap_syscall(long number, ...) {
    va_list args; va_start(args, number); long result;
    if (number == SYS_pidfd_open) {
        int pid = va_arg(args, int), flags = va_arg(args, int);
        result = __real_syscall(number, pid, flags);
    } else {
        assert(number == SYS_pidfd_send_signal);
        int fd = va_arg(args, int), signal = va_arg(args, int);
        void *info = va_arg(args, void *); int flags = va_arg(args, int);
        if (deny_cancel) { ++denied_pidfd; errno = EPERM; result = -1; }
        else result = __real_syscall(number, fd, signal, info, flags);
    }
    va_end(args); return result;
}
int __wrap_kill(pid_t pid, int number) {
    if (deny_cancel && pid == owned_pid) { ++denied_kill; errno = EPERM; return -1; }
    return __real_kill(pid, number);
}
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    if (denied_waits && pid == owned_pid) { --denied_waits; errno = EPERM; return -1; }
    return __real_waitpid(pid, status, options);
}
static void emit(int fd, const void *data, size_t size) {
    const char *next = data;
    while (size) {
        ssize_t count = write(fd, next, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) _exit(90);
        next += count; size -= (size_t)count;
    }
}
static char receive(int fd) {
    struct pollfd readable = {fd, POLLIN, 0}; int result;
    do { result = poll(&readable, 1, 5000); } while (result < 0 && errno == EINTR);
    assert(result == 1 && (readable.revents & POLLIN));
    char value; ssize_t count;
    do { count = read(fd, &value, 1); } while (count < 0 && errno == EINTR);
    assert(count == 1); return value;
}
static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    int count = 0; while (readdir(directory)) ++count;
    assert(closedir(directory) == 0); return count;
}
static void parent_alive(void) {
    assert(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() != 1);
    alarm(15);
}
static void latch(int number) { pending_signal = number; }
static void child_latch(int number) { (void)number; ++child_signal_count; }
typedef struct {
    struct sigaction interrupt, terminate, child;
    sigset_t original, blocked;
} guard;
static void guard_begin(guard *value) {
    assert(sigemptyset(&value->blocked) == 0);
    assert(sigaddset(&value->blocked, SIGINT) == 0);
    assert(sigaddset(&value->blocked, SIGTERM) == 0);
    assert(sigaddset(&value->blocked, SIGCHLD) == 0);
    assert(sigprocmask(SIG_BLOCK, &value->blocked, &value->original) == 0);
    assert(sigaction(SIGINT, NULL, &value->interrupt) == 0);
    assert(sigaction(SIGTERM, NULL, &value->terminate) == 0);
    assert(sigaction(SIGCHLD, NULL, &value->child) == 0);
    struct sigaction action = {0}; assert(sigemptyset(&action.sa_mask) == 0);
    action.sa_handler = SIG_DFL; assert(sigaction(SIGCHLD, &action, NULL) == 0);
    action.sa_handler = latch;
    assert(sigaction(SIGINT, &action, NULL) == 0);
    assert(sigaction(SIGTERM, &action, NULL) == 0);
    pending_signal = 0;
}
static int take_signal(const guard *value) {
    sigset_t previous;
    assert(sigprocmask(SIG_BLOCK, &value->blocked, &previous) == 0);
    int signal = pending_signal; pending_signal = 0;
    assert(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
    return signal;
}
static int guard_end(const guard *value) {
    assert(sigprocmask(SIG_BLOCK, &value->blocked, NULL) == 0);
    int signal = pending_signal;
    assert(sigaction(SIGINT, &value->interrupt, NULL) == 0);
    assert(sigaction(SIGTERM, &value->terminate, NULL) == 0);
    assert(sigaction(SIGCHLD, &value->child, NULL) == 0);
    assert(sigprocmask(SIG_SETMASK, &value->original, NULL) == 0);
    return signal;
}
static void assert_waitable(void) {
    struct sigaction action; assert(sigaction(SIGCHLD, NULL, &action) == 0);
    assert(action.sa_handler == SIG_DFL && !(action.sa_flags & SA_NOCLDWAIT));
}
static void fixture_child(const char *release) {
    parent_alive();
    sigset_t blocked, original;
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGINT) == 0 && sigaddset(&blocked, SIGTERM) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &original) == 0);
    struct sigaction action = {0}; action.sa_handler = child_latch;
    action.sa_mask = blocked;
    assert(sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGTERM, &action, NULL) == 0);
    emit(STDOUT_FILENO, "R", 1);
    while (!child_signal_count) assert(sigsuspend(&original) == -1 && errno == EINTR);
    assert(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
    emit(STDOUT_FILENO, "C", 1);
    while (access(release, F_OK) < 0) {
        assert(errno == ENOENT);
        struct timespec pause = {0, 1000000};
        while (nanosleep(&pause, &pause) < 0) assert(errno == EINTR);
    }
    emit(STDOUT_FILENO, "D1", 2);
    _exit(child_signal_count == 1 ? 0 : 91);
}
static void recover(pid_t pid, int events, int gate) {
    /* An application now owns this PID separately from the closed Child. */
    assert_waitable();
    assert(__real_kill(pid, SIGKILL) == 0);
    denied_waits = 2;
    int status; pid_t waited;
    for (;;) {
        waited = waitpid(pid, &status, 0);
        if (waited < 0 && errno == EINTR) continue;
        if (waited < 0 && errno == EPERM) {
            assert_waitable();
            emit(events, "W", 1); /* Diagnostic/retained scope, never return. */
            assert(receive(gate) == 'x');
            continue;
        }
        break;
    }
    assert(waited == pid && (WIFEXITED(status) || WIFSIGNALED(status)));
    /* Once terminal/ECHILD is observed, no further signaling is permitted. */
    assert(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
}
static void worker(const char *lock_path, const char *release, int events, int gate, bool transfer) {
    parent_alive();
    int lock = open(lock_path, O_RDWR | O_CLOEXEC); assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    struct sigaction ignored = {0}; ignored.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignored.sa_mask) == 0 && sigaction(SIGCHLD, &ignored, NULL) == 0);
    guard signals; guard_begin(&signals); assert_waitable();
    emit(events, "G", 1); assert(receive(gate) == 'x');
    uint8_t packed[PATH_MAX * 2 + 32]; size_t size = 0;
    const char *arguments[] = {executable, "--child", release};
    for (size_t i = 0; i < 3; ++i) {
        size_t length = strlen(arguments[i]) + 1; assert(length <= sizeof packed - size);
        memcpy(packed + size, arguments[i], length); size += length;
    }
    int64_t state[24] = {0}, meta[6], config[11] = {0, 1, 2, 1, -1, -1, -1, 0, 0, 64, 64};
    gp_metadata storage = {state, 24}, metadata = {meta, 6};
    gp_input empty = {NULL, 0}; uint8_t out[64]; int64_t done = 0;
    assert(gp_start((gp_input){packed, (int64_t)size}, empty, empty, empty, 0,
        (gp_metadata){config, 11}, storage, metadata) == 0);
    owned_pid = (pid_t)gp_id(storage); assert(owned_pid > 0);
    /* Keep startup signals blocked until ownership is registered. Waiting for
       the fixture's readiness makes graceful cleanup deterministic in this test. */
    while (!meta[3]) {
        assert(gp_wait(storage, empty, (gp_output){out, sizeof out}, (gp_output){NULL, 0},
                       20, 1, metadata, &done) == 0 && !done);
    }
    assert(meta[3] == 1 && out[0] == 'R'); emit(events, "R", 1);
    assert(sigprocmask(SIG_SETMASK, &signals.original, NULL) == 0);
    int cancellation = 0;
    if (transfer) {
        deny_cancel = true;
        assert(gp_close(storage, metadata) == 3 && meta[0] == EPERM && meta[5] == owned_pid);
        pid_t transferred = (pid_t)meta[5];
        assert(gp_close(storage, metadata) == 3 && meta[5] == 0);
        assert(denied_pidfd == 1 && denied_kill == 1 && gp_id(storage) == -1);
        deny_cancel = false;
        emit(events, "U", 1); assert(receive(gate) == 'x');
        recover(transferred, events, gate);
        assert(gp_close(storage, metadata) == 3 && meta[5] == 0);
    } else {
        size_t sent = 1;
        while (!done) {
            int signal = take_signal(&signals);
            if (signal) { cancellation = signal; assert(gp_signal(storage, signal, metadata) == 0); }
            assert(gp_wait(storage, empty, (gp_output){out, sizeof out}, (gp_output){NULL, 0},
                           20, 1, metadata, &done) == 0);
            assert(meta[5] == 0 && meta[3] >= (int64_t)sent && meta[3] <= (int64_t)sizeof out);
            emit(events, out + sent, (size_t)meta[3] - sent); sent = (size_t)meta[3];
        }
        assert(meta[1] == 0 && sent == 4 && !memcmp(out, "RCD1", 4));
        assert(gp_close(storage, metadata) == 0 && meta[5] == 0);
    }
    assert_waitable();
    int late = guard_end(&signals); if (late) cancellation = late;
    struct sigaction restored; assert(sigaction(SIGCHLD, NULL, &restored) == 0 && restored.sa_handler == SIG_IGN);
    assert(close(lock) == 0 && close(events) == 0 && close(gate) == 0);
    _exit(transfer ? 125 : 128 + cancellation);
}
static void assert_lock(const char *path, bool held) {
    int fd = open(path, O_RDWR | O_CLOEXEC); assert(fd >= 0);
    int result = flock(fd, LOCK_EX | LOCK_NB);
    if (held) assert(result < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
    else assert(result == 0);
    assert(close(fd) == 0);
}
static void scenario(bool startup, bool transfer) {
    int before = descriptors();
    char directory[] = "/tmp/goose-caller-XXXXXX", lock[128], release[128];
    assert(mkdtemp(directory));
    int length = snprintf(lock, sizeof lock, "%s/lock", directory);
    assert(length > 0 && length < (int)sizeof lock);
    int temporary = open(lock, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(temporary >= 0 && close(temporary) == 0);
    length = snprintf(release, sizeof release, "%s/release", directory);
    assert(length > 0 && length < (int)sizeof release);
    int events[2], gate[2]; assert(pipe2(events, O_CLOEXEC) == 0 && pipe2(gate, O_CLOEXEC) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(events[0]); close(gate[1]);
        worker(lock, release, events[1], gate[0], transfer);
    }
    assert(close(events[1]) == 0 && close(gate[0]) == 0);
    assert(receive(events[0]) == 'G'); assert_lock(lock, true);
    if (startup) assert(__real_kill(pid, SIGTERM) == 0);
    emit(gate[1], "x", 1);
    assert(receive(events[0]) == 'R'); assert_lock(lock, true);
    if (transfer) {
        assert(receive(events[0]) == 'U'); assert_lock(lock, true); emit(gate[1], "x", 1);
        for (int i = 0; i < 2; ++i) {
            assert(receive(events[0]) == 'W'); assert_lock(lock, true); emit(gate[1], "x", 1);
        }
    } else {
        if (!startup) assert(__real_kill(pid, SIGINT) == 0);
        assert(receive(events[0]) == 'C'); assert_lock(lock, true);
        int fd = open(release, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        assert(fd >= 0 && close(fd) == 0);
        assert(receive(events[0]) == 'D' && receive(events[0]) == '1');
    }
    int status; pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == pid && WIFEXITED(status));
    assert(WEXITSTATUS(status) == (transfer ? 125 : startup ? 143 : 130));
    assert_lock(lock, false);
    assert(close(events[0]) == 0 && close(gate[1]) == 0 && unlink(lock) == 0);
    if (!transfer) assert(unlink(release) == 0);
    assert(rmdir(directory) == 0);
    assert(descriptors() == before);
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--child")) fixture_child(argv[2]);
    assert(argc == 1); alarm(45);
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    assert(length > 0 && length < (ssize_t)sizeof executable - 1); executable[length] = 0;
    scenario(true, false); scenario(false, false); scenario(false, true);
    int status; assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("3 reference caller signal/lock/recovery scenarios passed");
    return 0;
}
