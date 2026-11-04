/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum Hook {
    NONE, ALLOCATE, OPEN, PIPE, DUP_FD, GET_FLAGS, SET_FLAGS, ACTION_INIT,
    ATTR_INIT, ATTR_MASK, ATTR_DEFAULT, ATTR_FLAGS, DUP_ACTION, CHDIR_ACTION,
    CLOSEFROM_ACTION, SPAWN, CLOCK, POLL, READ
};
static enum Hook target;
static int occurrence, seen, injected, fault_errno, diagnostics, stdio_mask;
static bool enabled, noisy_close;

static bool fail(enum Hook hook) {
    if (enabled && target == hook && ++seen == occurrence) {
        ++injected; errno = fault_errno; return true;
    }
    return false;
}
static void require(bool condition, const char *description) {
    if (!condition) {
        dprintf(diagnostics, "syscall test: %s (hook=%d occurrence=%d stdio=%d)\n",
                description, target, occurrence, stdio_mask);
        abort();
    }
}

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) { return fail(ALLOCATE) ? NULL : __real_malloc(size); }
int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    /* The native kernel opens only /dev/null without O_CREAT. */
    require(!(flags & O_CREAT), "unexpected creating open");
    return fail(OPEN) ? -1 : __real_open(path, flags);
}
int __real___open_2(const char *path, int flags);
int __wrap___open_2(const char *path, int flags) {
    require(!(flags & O_CREAT), "unexpected creating open");
    return fail(OPEN) ? -1 : __real___open_2(path, flags);
}
int __real_pipe2(int fds[2], int flags);
int __wrap_pipe2(int fds[2], int flags) {
    return fail(PIPE) ? -1 : __real_pipe2(fds, flags);
}
int __real_fcntl(int fd, int command, ...);
int __wrap_fcntl(int fd, int command, ...) {
    if (command == F_GETFL) return fail(GET_FLAGS) ? -1 : __real_fcntl(fd, command);
    require(command == F_DUPFD_CLOEXEC || command == F_SETFL, "unexpected fcntl command");
    va_list args; va_start(args, command); int argument = va_arg(args, int); va_end(args);
    return fail(command == F_DUPFD_CLOEXEC ? DUP_FD : SET_FLAGS) ? -1 :
        __real_fcntl(fd, command, argument);
}
int __real_close(int fd);
int __wrap_close(int fd) {
    int result = __real_close(fd);
    /* Linux close can release the descriptor and still report an error. */
    if (enabled && noisy_close && injected && result == 0) { errno = EINTR; return -1; }
    return result;
}
int __real_clock_gettime(clockid_t clock, struct timespec *time);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time) {
    return fail(CLOCK) ? -1 : __real_clock_gettime(clock, time);
}
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    return fail(POLL) ? -1 : __real_poll(fds, count, timeout);
}
/* Fortified builds can call these aliases instead of poll/read. Keep the same
   injection points while retaining libc's bounds checks on successful calls. */
int __real___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes);
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    return fail(POLL) ? -1 : __real___poll_chk(fds, count, timeout, bytes);
}
ssize_t __real_read(int fd, void *buffer, size_t length);
ssize_t __wrap_read(int fd, void *buffer, size_t length) {
    return fail(READ) ? -1 : __real_read(fd, buffer, length);
}
ssize_t __real___read_chk(int fd, void *buffer, size_t length, size_t bytes);
ssize_t __wrap___read_chk(int fd, void *buffer, size_t length, size_t bytes) {
    return fail(READ) ? -1 : __real___read_chk(fd, buffer, length, bytes);
}

/* These APIs return error numbers directly; deliberately poison errno to make
   accidental use of errno instead of the return value visible. */
#define ACTION_WRAPPER(name, hook, parameters, arguments) \
    int __real_##name parameters; \
    int __wrap_##name parameters { \
        if (fail(hook)) { errno = EBUSY; return fault_errno; } \
        return __real_##name arguments; \
    }
ACTION_WRAPPER(posix_spawn_file_actions_init, ACTION_INIT,
    (posix_spawn_file_actions_t *actions), (actions))
ACTION_WRAPPER(posix_spawnattr_init, ATTR_INIT,
    (posix_spawnattr_t *attributes), (attributes))
ACTION_WRAPPER(posix_spawnattr_setsigmask, ATTR_MASK,
    (posix_spawnattr_t *attributes, const sigset_t *set), (attributes, set))
ACTION_WRAPPER(posix_spawnattr_setsigdefault, ATTR_DEFAULT,
    (posix_spawnattr_t *attributes, const sigset_t *set), (attributes, set))
ACTION_WRAPPER(posix_spawnattr_setflags, ATTR_FLAGS,
    (posix_spawnattr_t *attributes, short flags), (attributes, flags))
ACTION_WRAPPER(posix_spawn_file_actions_adddup2, DUP_ACTION,
    (posix_spawn_file_actions_t *actions, int fd, int destination), (actions, fd, destination))
ACTION_WRAPPER(posix_spawn_file_actions_addchdir_np, CHDIR_ACTION,
    (posix_spawn_file_actions_t *actions, const char *path), (actions, path))
ACTION_WRAPPER(posix_spawn_file_actions_addclosefrom_np, CLOSEFROM_ACTION,
    (posix_spawn_file_actions_t *actions, int fd), (actions, fd))
ACTION_WRAPPER(posix_spawn, SPAWN,
    (pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
     const posix_spawnattr_t *attributes, char *const argv[], char *const environment[]),
    (pid, path, actions, attributes, argv, environment))
#undef ACTION_WRAPPER

static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); require(directory != NULL, "open descriptor list");
    int count = 0; while (readdir(directory)) ++count;
    require(closedir(directory) == 0, "close descriptor list");
    return count;
}

static void check_call(const char *helper, enum Hook hook, int nth, int error,
                       bool close_errors, bool transient) {
    uint8_t packed[8192], out[288], err[288];
    size_t length = strlen(helper) + 1;
    require(length <= 4096, "bounded helper path");
    memcpy(packed, helper, length); memcpy(packed + length, "bytes", sizeof "bytes");
    length += sizeof "bytes";
    memset(out, 0xa5, sizeof out); memset(err, 0x5a, sizeof err);
    int64_t meta[8] = {123, 0, -1, 0, 0, 0, 0, 456};
    target = hook; occurrence = nth; fault_errno = error;
    injected = 0; seen = 0; noisy_close = close_errors;
    int before = descriptors();
    enabled = true;
    int result = (int)gp_run((gp_input){packed, (int64_t)length}, (gp_input){(const uint8_t *)"/tmp", 4},
        2000, 1, (gp_output){out + 16, 256}, (gp_output){err + 16, 256}, (gp_metadata){meta + 1, 6});
    enabled = false;
    if (injected != (hook != NONE)) {
        dprintf(diagnostics, "injection diagnostics: seen=%d injected=%d result=%d "
                "os_error=%lld exit=%lld signal=%lld out=%lld err=%lld unreaped=%lld\n",
                seen, injected, result, (long long)meta[1], (long long)meta[2],
                (long long)meta[3], (long long)meta[4], (long long)meta[5], (long long)meta[6]);
    }
    require(injected == (hook != NONE), "selected failure was exercised");
    require(meta[0] == 123 && meta[7] == 456, "metadata canaries");
    require(meta[4] >= 0 && meta[4] <= 256 && meta[5] >= 0 && meta[5] <= 256, "capture lengths");
    require(meta[6] == 0, "no child ownership leaked");
    for (int i = 0; i < 16; ++i) {
        require(out[i] == 0xa5 && out[272 + i] == 0xa5, "stdout canaries");
        require(err[i] == 0x5a && err[272 + i] == 0x5a, "stderr canaries");
    }
    for (int64_t i = 0; i < meta[4]; ++i) require(out[16 + i] == i, "stdout prefix");
    for (int64_t i = 0; i < meta[5]; ++i) require(err[16 + i] == 255 - i, "stderr prefix");
    if (hook == NONE || transient) {
        require(result == 0 && meta[1] == 0 && meta[2] == 0 && meta[3] == 0, "successful retry");
        require(meta[4] == 256 && meta[5] == 256, "complete captures");
    } else {
        require(result == (hook == SPAWN ? 2 : 3), "error classification");
        require(meta[1] == (hook == ALLOCATE ? ENOMEM : error), "primary error preserved");
    }
    require(descriptors() == before, "exact descriptor count restored");
    int status; errno = 0;
    require(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "no live child or zombie");
}

static void matrix(const char *helper) {
    check_call(helper, NONE, 1, 0, false, false);
    const enum Hook single[] = {ALLOCATE, OPEN, ACTION_INIT, ATTR_INIT, ATTR_MASK,
        ATTR_DEFAULT, ATTR_FLAGS, CHDIR_ACTION, CLOSEFROM_ACTION, SPAWN, POLL, READ};
    for (size_t i = 0; i < sizeof single / sizeof single[0]; ++i)
        check_call(helper, single[i], 1, EIO, true, false);
    for (int nth = 1; nth <= 2; ++nth) {
        check_call(helper, PIPE, nth, EMFILE, true, false);
        check_call(helper, GET_FLAGS, nth, EIO, true, false);
        check_call(helper, SET_FLAGS, nth, EIO, true, false);
    }
    for (int nth = 1; nth <= 3; ++nth) {
        check_call(helper, DUP_ACTION, nth, ENOMEM, true, false);
        check_call(helper, CLOCK, nth, EIO, true, false);
    }
    /* Promotion releases the low slot, so both successive pipes reuse it. */
    int holes = !!(stdio_mask & 1) + !!(stdio_mask & 2) + !!(stdio_mask & 4);
    int promotions = holes ? 2 + 2 * (holes < 2 ? holes : 2) : 0;
    for (int nth = 1; nth <= promotions; ++nth)
        check_call(helper, DUP_FD, nth, EMFILE, true, false);
    check_call(helper, POLL, 1, EINTR, false, true);
    check_call(helper, READ, 1, EINTR, false, true);
    check_call(helper, READ, 1, EAGAIN, false, true);
}

int main(int argc, char **argv) {
    assert(argc == 2 && argv[1][0] == '/');
    int before = descriptors();
    diagnostics = __real_fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    assert(diagnostics >= 3);
    matrix(argv[1]);
    for (stdio_mask = 1; stdio_mask < 8; ++stdio_mask) {
        pid_t child = fork(); require(child >= 0, "fork closed-stdio case");
        if (child == 0) {
            for (int fd = 0; fd < 3; ++fd) if (stdio_mask & (1 << fd)) require(__real_close(fd) == 0, "close stdio");
            matrix(argv[1]); _exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "closed-stdio failure matrix");
    }
    assert(__real_close(diagnostics) == 0 && descriptors() == before);
    puts("syscall failure matrix passed for all eight stdio configurations");
    return 0;
}
