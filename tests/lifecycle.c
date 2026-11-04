/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { OK = 0, INVALID = 1, SYSTEM = 3, STATE = 6 };
typedef struct {
    int64_t state[24], config[11], meta[6];
    uint8_t out[64], err[64];
} fixture;

static char executable[PATH_MAX];
static bool synthetic_step, deny_signals;
static int clock_calls, poll_calls, poll_timeouts[4], denied_pidfd, denied_kill;
static int64_t clock_base;

int __real_clock_gettime(clockid_t clock, struct timespec *value);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __real___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes);
int __real_kill(pid_t pid, int signal);
long __real_syscall(long number, ...);

/* Two interrupted polls consume 6 ms and then 4 ms of one 10 ms slice.
   Extra calls fail rather than letting a regressed implementation hang. */
int __wrap_clock_gettime(clockid_t clock, struct timespec *value) {
    if (!synthetic_step || clock != CLOCK_MONOTONIC)
        return __real_clock_gettime(clock, value);
    static const int64_t offsets[] = {0, 0, 6000000, 10000000};
    if (clock_calls >= (int)(sizeof offsets / sizeof offsets[0])) {
        errno = EIO; return -1;
    }
    int64_t now = clock_base + offsets[clock_calls++];
    *value = (struct timespec){(time_t)(now / 1000000000), (long)(now % 1000000000)};
    return 0;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (!synthetic_step) return __real_poll(fds, count, timeout);
    if (poll_calls < 4) poll_timeouts[poll_calls] = timeout;
    ++poll_calls;
    if (!fds || count != 4 || poll_calls > 2) { errno = EIO; return -1; }
    errno = EINTR; return -1;
}

int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    if (!synthetic_step) return __real___poll_chk(fds, count, timeout, bytes);
    return __wrap_poll(fds, count, timeout);
}

long __wrap_syscall(long number, ...) {
    va_list args; va_start(args, number);
    long result;
    if (number == SYS_pidfd_open) {
        int pid = va_arg(args, int), flags = va_arg(args, int);
        result = __real_syscall(number, pid, flags);
    } else {
        assert(number == SYS_pidfd_send_signal);
        int fd = va_arg(args, int), signal = va_arg(args, int);
        void *info = va_arg(args, void *); int flags = va_arg(args, int);
        if (deny_signals) { ++denied_pidfd; errno = EPERM; result = -1; }
        else result = __real_syscall(number, fd, signal, info, flags);
    }
    va_end(args); return result;
}

int __wrap_kill(pid_t pid, int signal) {
    if (deny_signals) { ++denied_kill; errno = EPERM; return -1; }
    return __real_kill(pid, signal);
}

static gp_metadata words(int64_t *data, int64_t length) {
    return (gp_metadata){data, length};
}
static gp_input empty(void) { return (gp_input){NULL, 0}; }
static gp_metadata state(fixture *f) { return words(f->state, 24); }
static gp_metadata meta(fixture *f) { return words(f->meta, 6); }

static fixture fresh(void) {
    fixture f = {.config = {60000, 1, 1, 0, -1, -1, -1, 0, 0, 64, 64}};
    memset(f.out, 0xa5, sizeof f.out); memset(f.err, 0x5a, sizeof f.err);
    return f;
}

static int start(fixture *f, const char *mode) {
    uint8_t packed[PATH_MAX + 32];
    size_t first = strlen(executable) + 1, second = strlen(mode) + 1;
    assert(first + second <= sizeof packed);
    memcpy(packed, executable, first); memcpy(packed + first, mode, second);
    return (int)gp_start((gp_input){packed, (int64_t)(first + second)}, empty(), empty(), empty(),
        0, words(f->config, 11), state(f), meta(f));
}

static int collect(fixture *f, int64_t slice_ms, bool step, int64_t *done) {
    return (int)gp_wait(state(f), empty(), (gp_output){f->out, sizeof f->out},
        (gp_output){f->err, sizeof f->err}, slice_ms, step, meta(f), done);
}

static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    int count = 0;
    while (readdir(directory)) ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void assert_reaped(pid_t pid) {
    int status;
    assert(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void interrupted_step(void) {
    int before = descriptors();
    fixture f = fresh(); assert(start(&f, "hold") == OK);
    pid_t pid = (pid_t)gp_id(state(&f));
    struct timespec now; assert(__real_clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    clock_base = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    clock_calls = 0; poll_calls = 0; synthetic_step = true;
    int64_t done = -1;
    int result = collect(&f, 10, true, &done);
    synthetic_step = false;
    int closed = (int)gp_close(state(&f), meta(&f));
    assert_reaped(pid);
    assert(result == OK && done == 0 && closed == OK);
    assert(clock_calls == 4 && poll_calls == 2);
    assert(poll_timeouts[0] == 10 && poll_timeouts[1] == 4);
    assert(descriptors() == before);
}

static void retained_capture_and_reuse(void) {
    static const uint8_t stdout_bytes[] = {'o', 'u', 't', 0, 0xff};
    static const uint8_t stderr_bytes[] = {'e', 'r', 'r', 0, 0xfe};
    int before = descriptors(); fixture f = fresh();
    assert(gp_close(state(&f), meta(&f)) == OK && gp_id(state(&f)) == -1);
    assert(start(&f, "capture") == OK);
    pid_t pid = (pid_t)gp_id(state(&f));
    assert(start(&f, "hold") == STATE && gp_id(state(&f)) == pid);
    /* Observe terminal state without consuming it or touching capture pipes. */
    siginfo_t observed;
    int result;
    do { result = waitid(P_PID, (id_t)pid, &observed, WEXITED | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    assert(result == 0 && observed.si_pid == pid);
    int64_t done = -1;
    assert(gp_try_wait(state(&f), meta(&f), &done) == OK && done == 1);
    assert(f.meta[1] == 0 && f.meta[3] == 0 && f.meta[4] == 0);
    for (size_t i = 0; i < sizeof f.out; ++i) assert(f.out[i] == 0xa5 && f.err[i] == 0x5a);
    assert(gp_signal(state(&f), SIGTERM, meta(&f)) == STATE);
    assert(collect(&f, 0, false, &done) == OK && done == 1);
    assert(f.meta[1] == 0 && f.meta[3] == 5 && f.meta[4] == 5);
    assert(memcmp(f.out, stdout_bytes, sizeof stdout_bytes) == 0);
    assert(memcmp(f.err, stderr_bytes, sizeof stderr_bytes) == 0);
    assert(collect(&f, 0, false, &done) == OK && done == 1 && f.meta[3] == 5 && f.meta[4] == 5);
    assert(gp_close(state(&f), meta(&f)) == OK && f.meta[5] == 0);
    assert(gp_close(state(&f), meta(&f)) == OK && f.meta[5] == 0);
    assert_reaped(pid);
    assert(start(&f, "hold") == OK);
    pid = (pid_t)gp_id(state(&f));
    assert(gp_try_wait(state(&f), meta(&f), &done) == OK && done == 0);
    assert(gp_signal(state(&f), SIGTERM, meta(&f)) == OK);
    assert(collect(&f, 0, false, &done) == OK && done == 1);
    assert(f.meta[1] == -1 && f.meta[2] == SIGTERM && f.meta[3] == 0 && f.meta[4] == 0);
    assert(gp_id(state(&f)) == -1 && gp_can_start(state(&f)) == 1);
    assert_reaped(pid);
    assert(descriptors() == before);
}

static void ownership_transfer_once(void) {
    int before = descriptors(); fixture f = fresh();
    assert(start(&f, "hold") == OK);
    pid_t pid = (pid_t)gp_id(state(&f));
    denied_pidfd = 0; denied_kill = 0; deny_signals = true;
    int result = (int)gp_close(state(&f), meta(&f));
    int64_t transferred = f.meta[5], error = f.meta[0];
    int again = (int)gp_close(state(&f), meta(&f));
    int64_t repeated_transfer = f.meta[5];
    deny_signals = false;
    /* The transferred child is now exclusively this test's responsibility. */
    assert(__real_kill(pid, SIGKILL) == 0);
    int status; pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(result == SYSTEM && error == EPERM && transferred == pid);
    assert(again == SYSTEM && repeated_transfer == 0 && denied_pidfd == 1 && denied_kill == 1);
    assert(gp_id(state(&f)) == -1 && gp_can_start(state(&f)) == 1);
    int64_t done = -1;
    assert(collect(&f, 0, false, &done) == SYSTEM && done == 1 && f.meta[5] == 0);
    assert(start(&f, "capture") == OK);
    assert(collect(&f, 0, false, &done) == OK && done == 1 && f.meta[1] == 0);
    assert(descriptors() == before);
}

static void refused_aliases_preserved(void) {
    fixture f = fresh();
    int64_t alias[11], saved[11], original_state[24];
    memset(alias, 0x5a, sizeof alias);
    memcpy(alias, "/bin/true", sizeof "/bin/true");
    memcpy(saved, alias, sizeof alias); memcpy(original_state, f.state, sizeof original_state);
    assert(gp_start((gp_input){(uint8_t *)alias, sizeof "/bin/true"}, empty(), empty(), empty(),
        0, words(f.config, 11), state(&f), words(alias, 6)) == INVALID);
    assert(memcmp(alias, saved, sizeof alias) == 0);
    memcpy(alias, f.config, sizeof alias); memcpy(saved, alias, sizeof alias);
    assert(gp_start(empty(), empty(), empty(), empty(), 0, words(alias, 11),
        state(&f), words(alias, 6)) == INVALID);
    assert(memcmp(alias, saved, sizeof alias) == 0);
    int64_t done = 91;
    assert(gp_wait(state(&f), empty(), (gp_output){(uint8_t *)alias, sizeof alias},
        (gp_output){NULL, 0}, 0, 1, words(alias, 6), &done) == INVALID);
    assert(done == 91 && memcmp(alias, saved, sizeof alias) == 0);
    assert(gp_wait(state(&f), (gp_input){(uint8_t *)alias, sizeof alias}, (gp_output){NULL, 0},
        (gp_output){NULL, 0}, 0, 1, words(alias, 6), &done) == INVALID);
    assert(done == 91 && memcmp(alias, saved, sizeof alias) == 0);
    assert(gp_wait(state(&f), empty(), (gp_output){NULL, 0}, (gp_output){NULL, 0},
        0, 1, words(alias, 6), &alias[2]) == INVALID);
    assert(memcmp(alias, saved, sizeof alias) == 0);
    assert(gp_try_wait(state(&f), words(alias, 6), &alias[2]) == INVALID);
    assert(memcmp(alias, saved, sizeof alias) == 0);
    assert(gp_close(state(&f), words(f.state, 6)) == INVALID);
    assert(memcmp(f.state, original_state, sizeof original_state) == 0);
}

static void write_bytes(int fd, const uint8_t *bytes, size_t size) {
    while (size) {
        ssize_t count = write(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) _exit(80);
        bytes += count; size -= (size_t)count;
    }
}

int main(int argc, char **argv) {
    if (argc == 2) {
        /* A failing parent assertion must not strand its fixture child. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() == 1) return 81;
        alarm(10);
        if (strcmp(argv[1], "capture") == 0) {
            static const uint8_t out[] = {'o', 'u', 't', 0, 0xff};
            static const uint8_t err[] = {'e', 'r', 'r', 0, 0xfe};
            write_bytes(STDOUT_FILENO, out, sizeof out); write_bytes(STDERR_FILENO, err, sizeof err);
            return 0;
        }
        assert(strcmp(argv[1], "hold") == 0);
        for (;;) pause();
    }
    alarm(30);
    ssize_t size = readlink("/proc/self/exe", executable, sizeof executable - 1);
    assert(size > 0 && size < (ssize_t)sizeof executable - 1); executable[size] = 0;
    interrupted_step();
    retained_capture_and_reuse();
    ownership_transfer_once();
    refused_aliases_preserved();
    int status; assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("4 child lifecycle regression groups passed");
    return 0;
}
