/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { OK, INVALID, LAUNCH, SYSTEM, TIMEOUT };
enum { CONSUMER, PRODUCER, STAGES };
/* These are the documented ABI sizes, not copies of the private C structs. */
enum { STATE_WORDS = 55, META_WORDS = 17, CONFIG_WORDS = 11 };
static const char *helper;
static struct {
    pid_t pid[STAGES];
    int native_pidfd[STAGES], recovery_pidfd[STAGES];
    unsigned kill_requested, wait_faults, signal_faults;
    int spawn_calls, fail_spawn, fail_pidfd, io_calls, signal_denials[STAGES];
    int wait_denials[STAGES], clock_calls, blocking_waits;
    bool active, check_order, fake_clock, interrupt_poll;
    int64_t elapsed_ns, spawn_advance_ns;
} fixture;

long __real_syscall(long number, ...);
int __real_kill(pid_t pid, int number);
pid_t __real_waitpid(pid_t pid, int *status, int options);
int __real_clock_gettime(clockid_t clock, struct timespec *value);
int __real_pipe2(int fds[2], int flags);
int __real_socketpair(int domain, int type, int protocol, int fds[2]);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __real___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes);
ssize_t __real_read(int fd, void *data, size_t length);
ssize_t __real_send(int fd, const void *data, size_t length, int flags);
int __real_posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[]);

static int owned(pid_t pid) {
    for (int i = 0; i < STAGES; ++i) if (pid > 0 && pid == fixture.pid[i]) return i;
    return -1;
}
static int owned_fd(int fd) {
    for (int i = 0; i < STAGES; ++i)
        if (fd >= 0 && fd == fixture.native_pidfd[i]) return i;
    return -1;
}
static pid_t terminal_wait(pid_t pid, int *status) {
    pid_t result;
    do { result = __real_waitpid(pid, status, 0); } while (result < 0 && errno == EINTR);
    return result;
}
/* Failure cleanup uses the separate test owner's pidfds, never a foreign PID. */
static void emergency_cleanup(void) {
    fixture.active = false;
    for (int i = 0; i < STAGES; ++i) {
        if (fixture.pid[i] > 0) {
            int status;
            pid_t result = __real_waitpid(fixture.pid[i], &status, WNOHANG);
            if (result == 0) {
                if (fixture.recovery_pidfd[i] >= 0)
                    (void)__real_syscall(SYS_pidfd_send_signal, fixture.recovery_pidfd[i], SIGKILL, NULL, 0);
                else (void)__real_kill(fixture.pid[i], SIGKILL);
                (void)terminal_wait(fixture.pid[i], &status);
            }
        }
        if (fixture.recovery_pidfd[i] >= 0) (void)close(fixture.recovery_pidfd[i]);
        fixture.recovery_pidfd[i] = -1;
    }
}
static void failed(const char *expression, int line) {
    int saved = errno;
    emergency_cleanup();
    fprintf(stderr, "pipeline.c:%d: %s (errno=%d)\n", line, expression, saved);
    exit(1);
}
#define CHECK(expression) do { if (!(expression)) failed(#expression, __LINE__); } while (0)

int __wrap_posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[]) {
    if (!fixture.active) return __real_posix_spawn(pid, path, actions, attributes, argv, environment);
    int index = fixture.spawn_calls++;
    CHECK(index < STAGES);
    CHECK(strcmp(path, helper) == 0 && strcmp(argv[0], helper) == 0);
    if (fixture.fail_spawn == index + 1) return ENOENT;
    int result = __real_posix_spawn(pid, path, actions, attributes, argv, environment);
    if (!result) {
        fixture.pid[index] = *pid;
        fixture.recovery_pidfd[index] = (int)__real_syscall(SYS_pidfd_open, *pid, 0);
        CHECK(fixture.recovery_pidfd[index] >= 0);
        if (fixture.fake_clock) fixture.elapsed_ns += fixture.spawn_advance_ns;
    }
    return result;
}
long __wrap_syscall(long number, ...) {
    va_list args; va_start(args, number);
    long result;
    if (number == SYS_pidfd_open) {
        int pid = va_arg(args, int), flags = va_arg(args, int);
        int index = owned((pid_t)pid);
        if (fixture.active) CHECK(index >= 0);
        if (fixture.active && fixture.fail_pidfd == index + 1) {
            errno = EMFILE; result = -1;
        } else {
            result = __real_syscall(number, pid, flags);
            if (fixture.active && result >= 0) fixture.native_pidfd[index] = (int)result;
        }
    } else {
        CHECK(number == SYS_pidfd_send_signal);
        int fd = va_arg(args, int), number_signal = va_arg(args, int);
        void *info = va_arg(args, void *); int flags = va_arg(args, int);
        int index = owned_fd(fd);
        if (fixture.active) {
            CHECK(index >= 0 && number_signal == SIGKILL);
            fixture.kill_requested |= 1u << index;
        }
        if (fixture.active && (fixture.signal_faults & (1u << index))) {
            ++fixture.signal_denials[index]; errno = EPERM; result = -1;
        } else result = __real_syscall(number, fd, number_signal, info, flags);
    }
    va_end(args); return result;
}
int __wrap_kill(pid_t pid, int number) {
    int index = owned(pid);
    if (fixture.active) {
        CHECK(index >= 0 && number == SIGKILL);
        fixture.kill_requested |= 1u << index;
        if (fixture.signal_faults & (1u << index)) {
            ++fixture.signal_denials[index]; errno = EPERM; return -1;
        }
    }
    return __real_kill(pid, number);
}
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    int index = owned(pid);
    if (fixture.active && index >= 0) {
        if (options == 0) {
            ++fixture.blocking_waits;
            if (fixture.check_order) {
                unsigned expected = 0;
                for (int i = 0; i < STAGES; ++i) if (fixture.pid[i] > 0) expected |= 1u << i;
                CHECK((fixture.kill_requested & expected) == expected);
            }
        }
        if (fixture.wait_faults & (1u << index)) {
            CHECK(fixture.kill_requested & (1u << index));
            ++fixture.wait_denials[index];
            errno = index == PRODUCER ? EACCES : EPERM; return -1;
        }
    }
    return __real_waitpid(pid, status, options);
}
int __wrap_clock_gettime(clockid_t clock, struct timespec *value) {
    if (fixture.active) ++fixture.clock_calls;
    if (!fixture.active || !fixture.fake_clock || clock != CLOCK_MONOTONIC)
        return __real_clock_gettime(clock, value);
    *value = (struct timespec){1000, (long)fixture.elapsed_ns}; return 0;
}
int __wrap_pipe2(int fds[2], int flags) {
    if (fixture.active) ++fixture.io_calls;
    return __real_pipe2(fds, flags);
}
int __wrap_socketpair(int domain, int type, int protocol, int fds[2]) {
    if (fixture.active) ++fixture.io_calls;
    return __real_socketpair(domain, type, protocol, fds);
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (fixture.active) {
        ++fixture.io_calls;
        if (fixture.interrupt_poll) {
            fixture.interrupt_poll = false; fixture.elapsed_ns += 11000000;
            errno = EINTR; return -1;
        }
    }
    return __real_poll(fds, count, timeout);
}
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    if (fixture.active) {
        (void)bytes; return __wrap_poll(fds, count, timeout);
    }
    return __real___poll_chk(fds, count, timeout, bytes);
}
ssize_t __wrap_read(int fd, void *data, size_t length) {
    if (fixture.active) ++fixture.io_calls;
    return __real_read(fd, data, length);
}
ssize_t __wrap_send(int fd, const void *data, size_t length, int flags) {
    if (fixture.active) ++fixture.io_calls;
    return __real_send(fd, data, length, flags);
}

static gp_input empty(void) { return (gp_input){NULL, 0}; }
static gp_metadata words(int64_t *data, int64_t length) { return (gp_metadata){data, length}; }
typedef struct {
    int64_t state[STATE_WORDS], meta[META_WORDS], first[CONFIG_WORDS], second[CONFIG_WORDS];
    uint8_t first_command[4096], second_command[4096];
    gp_input command[STAGES];
    uint8_t out[64], first_err[64], second_err[64];
} owner;
static gp_input command(uint8_t *buffer, size_t capacity, const char *mode,
                        const char *arg, const char *arg2) {
    const char *parts[] = {helper, mode, arg, arg2}; size_t length = 0;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
        if (!parts[i]) continue;
        size_t take = strlen(parts[i]) + 1;
        CHECK(take <= capacity - length);
        memcpy(buffer + length, parts[i], take); length += take;
    }
    return (gp_input){buffer, (int64_t)length};
}
static void initialize(owner *value) {
    memset(value, 0, sizeof *value);
    int64_t config[CONFIG_WORDS] = {5000, 1, 1, 0, -1, -1, -1, 0, 0, 64, 64};
    memcpy(value->first, config, sizeof config); memcpy(value->second, config, sizeof config);
    value->command[PRODUCER] = command(value->first_command, sizeof value->first_command, "idle", NULL, NULL);
    value->command[CONSUMER] = command(value->second_command, sizeof value->second_command, "idle", NULL, NULL);
}
static int start(owner *value) {
    return (int)gp_pipeline_start(value->command[PRODUCER], empty(), empty(), empty(),
        value->command[CONSUMER], empty(), empty(), empty(), 0,
        words(value->first, CONFIG_WORDS), words(value->second, CONFIG_WORDS),
        words(value->state, STATE_WORDS), words(value->meta, META_WORDS));
}
static int close_owner(owner *value) {
    return (int)gp_pipeline_close(words(value->state, STATE_WORDS), words(value->meta, META_WORDS));
}
static int wait_owner(owner *value, int64_t *done) {
    return (int)gp_pipeline_wait(words(value->state, STATE_WORDS), empty(),
        (gp_output){value->out, sizeof value->out},
        (gp_output){value->first_err, sizeof value->first_err},
        (gp_output){value->second_err, sizeof value->second_err}, 0, 0,
        words(value->meta, META_WORDS), done);
}
static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); CHECK(directory);
    int count = 0;
    while (readdir(directory)) ++count;
    CHECK(closedir(directory) == 0); return count;
}
static void begin(void) {
    memset(&fixture, 0, sizeof fixture);
    for (int i = 0; i < STAGES; ++i)
        fixture.native_pidfd[i] = fixture.recovery_pidfd[i] = -1;
    fixture.active = true; fixture.check_order = true; fixture.spawn_advance_ns = 7000000;
}
static void no_children(void) {
    for (int i = 0; i < STAGES; ++i) if (fixture.pid[i] > 0) {
        int status;
        CHECK(__real_waitpid(fixture.pid[i], &status, WNOHANG) == -1 && errno == ECHILD);
    }
    int status; CHECK(__real_waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}
static void end(int before) {
    no_children(); fixture.active = false;
    for (int i = 0; i < STAGES; ++i) {
        if (fixture.recovery_pidfd[i] >= 0) CHECK(close(fixture.recovery_pidfd[i]) == 0);
        fixture.recovery_pidfd[i] = -1;
    }
    CHECK(descriptors() == before);
}
static void no_transfers(const owner *value) { CHECK(value->meta[8] == 0 && value->meta[14] == 0); }
static void killed(const owner *value, bool first, bool second) {
    CHECK(value->meta[15] == first && value->meta[16] == second);
    if (first) CHECK(value->meta[4] == -1 && value->meta[5] == SIGKILL);
    if (second) CHECK(value->meta[10] == -1 && value->meta[11] == SIGKILL);
    no_transfers(value);
}
static void ordinary_close(void) {
    int before = descriptors(); begin(); owner value; initialize(&value);
    CHECK(start(&value) == OK && fixture.spawn_calls == STAGES);
    CHECK(fixture.pid[CONSUMER] > 0 && fixture.pid[PRODUCER] > 0);
    CHECK(fixture.pid[CONSUMER] != fixture.pid[PRODUCER]);
    CHECK(close_owner(&value) == OK); killed(&value, true, true);
    CHECK(fixture.kill_requested == 3 && fixture.blocking_waits == STAGES);
    CHECK(value.meta[1] == OK && value.meta[2] == OK && value.meta[3] == 0 && value.meta[9] == 0);
    CHECK(close_owner(&value) == OK); no_transfers(&value);
    CHECK(gp_pipeline_can_start(words(value.state, STATE_WORDS)));
    end(before);
}
static void launch_failures(void) {
    for (int target = 1; target <= STAGES; ++target) {
        int before = descriptors(); begin(); fixture.fail_spawn = target;
        owner value; initialize(&value);
        CHECK(start(&value) == LAUNCH && value.meta[0] == ENOENT);
        CHECK(fixture.spawn_calls == target);
        CHECK(value.meta[15] == 0 && value.meta[16] == (target == 2));
        CHECK(value.meta[target == 1 ? 2 : 1] == LAUNCH);
        CHECK(value.meta[target == 1 ? 9 : 3] == ENOENT);
        CHECK(value.meta[4] == -1);
        if (target == 2) CHECK(value.meta[11] == SIGKILL);
        else CHECK(value.meta[10] == -1 && fixture.kill_requested == 0);
        no_transfers(&value);
        CHECK(close_owner(&value) == LAUNCH); no_transfers(&value); end(before);
    }
}
static void postspawn_failure(void) {
    int before = descriptors(); begin(); fixture.fail_pidfd = PRODUCER + 1;
    owner value; initialize(&value);
    CHECK(start(&value) == SYSTEM && value.meta[0] == EMFILE);
    CHECK(fixture.spawn_calls == STAGES && fixture.kill_requested == 3);
    CHECK(value.meta[1] == SYSTEM && value.meta[3] == EMFILE);
    CHECK(value.meta[2] == OK && value.meta[9] == 0);
    killed(&value, true, true);
    CHECK(close_owner(&value) == SYSTEM); no_transfers(&value); end(before);
}
static void interrupted_deadline(void) {
    int before = descriptors(); begin(); fixture.fake_clock = true;
    fixture.spawn_advance_ns = 0;
    owner value; initialize(&value); value.first[0] = value.second[0] = 10;
    CHECK(start(&value) == OK && fixture.spawn_calls == STAGES);
    fixture.interrupt_poll = true;
    int64_t done = 0; CHECK(wait_owner(&value, &done) == TIMEOUT && done == 1);
    CHECK(!fixture.interrupt_poll && fixture.elapsed_ns == 11000000);
    CHECK(value.meta[0] == 0 && value.meta[1] == TIMEOUT && value.meta[2] == TIMEOUT);
    killed(&value, true, true); end(before);
}
static void shared_deadline(void) {
    int before = descriptors(); begin(); fixture.fake_clock = true;
    owner value; initialize(&value); value.first[0] = value.second[0] = 10;
    CHECK(start(&value) == TIMEOUT && fixture.spawn_calls == STAGES);
    CHECK(fixture.elapsed_ns == 14000000);
    int64_t done = 0; CHECK(wait_owner(&value, &done) == TIMEOUT && done == 1);
    CHECK(value.meta[0] == 0 && value.meta[1] == TIMEOUT && value.meta[2] == TIMEOUT);
    CHECK(fixture.kill_requested == 3); killed(&value, true, true); end(before);
}
/* Denials are specific to actual fixture children, after successful SIGKILL.
   Different errno values ensure the two diagnostic slots are independent. */
static void denied_waits(void) {
    for (unsigned mask = 1; mask <= 3; ++mask) {
        int before = descriptors(); begin(); owner value; initialize(&value);
        CHECK(start(&value) == OK); fixture.wait_faults = mask;
        CHECK(close_owner(&value) == SYSTEM && fixture.kill_requested == 3);
        CHECK(value.meta[15] == 1 && value.meta[16] == 1);
        pid_t first = (pid_t)value.meta[8], second = (pid_t)value.meta[14];
        CHECK(first == ((mask & (1u << PRODUCER)) ? fixture.pid[PRODUCER] : 0));
        CHECK(second == ((mask & (1u << CONSUMER)) ? fixture.pid[CONSUMER] : 0));
        if (mask == 3) CHECK(first > 0 && second > 0 && first != second);
        for (int i = 0; i < STAGES; ++i) {
            int result = i == PRODUCER ? 1 : 2, metadata = i == PRODUCER ? 3 : 9;
            bool denied = (mask & (1u << i)) != 0;
            CHECK(value.meta[result] == (denied ? SYSTEM : OK));
            CHECK(value.meta[metadata] == (denied ? (i == PRODUCER ? EACCES : EPERM) : 0));
            CHECK(value.meta[metadata + 1] == -1);
            CHECK(value.meta[metadata + 2] == (denied ? 0 : SIGKILL));
            CHECK(fixture.wait_denials[i] == (denied ? 1 : 0));
        }
        CHECK(value.meta[0] == ((mask & (1u << PRODUCER)) ? EACCES : EPERM));
        CHECK(close_owner(&value) == SYSTEM); no_transfers(&value);
        CHECK(close_owner(&value) == SYSTEM); no_transfers(&value);
        CHECK(gp_pipeline_can_start(words(value.state, STATE_WORDS)));
        fixture.wait_faults = 0;
        for (int i = 0; i < STAGES; ++i) if (mask & (1u << i)) {
            int status; CHECK(terminal_wait(fixture.pid[i], &status) == fixture.pid[i]);
            CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        }
        end(before);
    }
}
static void denied_signals(void) {
    int before = descriptors(); begin(); owner value; initialize(&value);
    CHECK(start(&value) == OK);
    CHECK(fixture.recovery_pidfd[0] >= 0 && fixture.recovery_pidfd[1] >= 0);
    fixture.signal_faults = 3;
    CHECK(close_owner(&value) == SYSTEM && value.meta[0] == EPERM);
    CHECK(value.meta[8] == fixture.pid[PRODUCER] && value.meta[14] == fixture.pid[CONSUMER]);
    CHECK(value.meta[8] > 0 && value.meta[14] > 0 && value.meta[8] != value.meta[14]);
    CHECK(fixture.blocking_waits == 0 && fixture.kill_requested == 3);
    for (int i = 0; i < STAGES; ++i) {
        CHECK(fixture.signal_denials[i] == 2);
        int metadata = i == PRODUCER ? 3 : 9;
        CHECK(value.meta[i == PRODUCER ? 1 : 2] == SYSTEM && value.meta[metadata] == EPERM);
        CHECK(value.meta[metadata + 1] == -1 && value.meta[metadata + 2] == 0);
        int status; CHECK(__real_waitpid(fixture.pid[i], &status, WNOHANG) == 0);
    }
    CHECK(close_owner(&value) == SYSTEM); no_transfers(&value);
    CHECK(close_owner(&value) == SYSTEM); no_transfers(&value);
    /* The terminal pipeline cannot recover transfers. The test's held pidfds do. */
    for (int i = 0; i < STAGES; ++i)
        CHECK(__real_syscall(SYS_pidfd_send_signal, fixture.recovery_pidfd[i], SIGKILL, NULL, 0) == 0);
    for (int i = 0; i < STAGES; ++i) {
        int status; CHECK(terminal_wait(fixture.pid[i], &status) == fixture.pid[i]);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    }
    end(before);
}
static void validation(void) {
    int before = descriptors(); begin(); owner value; initialize(&value);
    owner snapshot = value;
    /* Outer aliases are rejected before either output storage is touched. */
    CHECK(gp_pipeline_start(value.command[PRODUCER], empty(), empty(), empty(),
        value.command[CONSUMER], empty(), empty(), empty(), 0,
        words(value.first, CONFIG_WORDS), words(value.second, CONFIG_WORDS),
        words(value.state, STATE_WORDS), words(value.state + 1, META_WORDS)) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0);
    CHECK(gp_pipeline_start(value.command[PRODUCER], empty(), empty(), empty(),
        value.command[CONSUMER], empty(), empty(), empty(), 0,
        words(value.state, CONFIG_WORDS), words(value.second, CONFIG_WORDS),
        words(value.state, STATE_WORDS), words(value.meta, META_WORDS)) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0);
    CHECK(gp_pipeline_start(value.command[PRODUCER], empty(), empty(), empty(),
        (gp_input){(const uint8_t *)value.meta, 1}, empty(), empty(), empty(), 0,
        words(value.first, CONFIG_WORDS), words(value.second, CONFIG_WORDS),
        words(value.state, STATE_WORDS), words(value.meta, META_WORDS)) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0);
    CHECK(gp_pipeline_close(words(value.state, STATE_WORDS), words(value.state, META_WORDS)) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0);
    CHECK(fixture.spawn_calls == 0 && fixture.io_calls == 0 && fixture.clock_calls == 0);
    for (int stage = 0; stage < STAGES; ++stage) {
        initialize(&value);
        value.command[stage] = (gp_input){NULL, 1};
        CHECK(start(&value) == INVALID);
        CHECK(fixture.spawn_calls == 0 && fixture.io_calls == 0);
        no_transfers(&value);
    }
    initialize(&value); value.command[CONSUMER].len = -1;
    CHECK(start(&value) == INVALID && fixture.spawn_calls == 0 && fixture.io_calls == 0);
    initialize(&value); value.second[3] = 2; /* Input.Bytes is invalid for the consumer. */
    CHECK(start(&value) == INVALID && fixture.spawn_calls == 0 && fixture.io_calls == 0);
    initialize(&value); CHECK(start(&value) == OK);
    int io_before = fixture.io_calls, clocks_before = fixture.clock_calls;
    int64_t done = 99;
    snapshot = value;
    CHECK(gp_pipeline_wait(words(value.state, STATE_WORDS), empty(),
        (gp_output){(uint8_t *)value.meta, 64},
        (gp_output){value.first_err, 64}, (gp_output){value.second_err, 64},
        0, 0, words(value.meta, META_WORDS), &done) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0 && done == 99);
    CHECK(gp_pipeline_wait(words(value.state, STATE_WORDS), empty(),
        (gp_output){value.out, 64}, (gp_output){value.out + 1, 63},
        (gp_output){value.second_err, 64}, 0, 0, words(value.meta, META_WORDS), &done) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0 && done == 99);
    CHECK(gp_pipeline_wait(words(value.state, STATE_WORDS), empty(),
        (gp_output){value.out, -1}, (gp_output){value.first_err, 64},
        (gp_output){value.second_err, 64}, 0, 0, words(value.meta, META_WORDS), &done) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0 && done == 99);
    CHECK(gp_pipeline_wait(words(value.state, STATE_WORDS), (gp_input){NULL, 1},
        (gp_output){value.out, 64}, (gp_output){value.first_err, 64},
        (gp_output){value.second_err, 64}, 0, 0, words(value.meta, META_WORDS), &done) == INVALID);
    CHECK(memcmp(&value, &snapshot, sizeof value) == 0 && done == 99);
    CHECK(gp_pipeline_wait(words(value.state, STATE_WORDS), empty(),
        (gp_output){value.out, 63}, (gp_output){value.first_err, 64},
        (gp_output){value.second_err, 64}, 0, 0, words(value.meta, META_WORDS), &done) == INVALID);
    CHECK(memcmp(value.state, snapshot.state, sizeof value.state) == 0 && done == 99);
    CHECK(fixture.io_calls == io_before && fixture.clock_calls == clocks_before);
    CHECK(close_owner(&value) == OK); killed(&value, true, true); end(before);
}
static void exit_statuses_and_reuse(void) {
    int before = descriptors(); begin(); owner value; initialize(&value);
    value.command[PRODUCER] = command(value.first_command, sizeof value.first_command, "exit", "23", NULL);
    value.command[CONSUMER] = command(value.second_command, sizeof value.second_command, "exit", "42", NULL);
    CHECK(start(&value) == OK);
    int64_t done = 0; CHECK(wait_owner(&value, &done) == OK && done == 1);
    CHECK(value.meta[0] == 0 && value.meta[1] == OK && value.meta[2] == OK);
    CHECK(value.meta[3] == 0 && value.meta[4] == 23 && value.meta[5] == 0);
    CHECK(value.meta[9] == 0 && value.meta[10] == 42 && value.meta[11] == 0);
    CHECK(value.meta[15] == 1 && value.meta[16] == 1); no_transfers(&value);
    CHECK(fixture.kill_requested == 0);
    CHECK(close_owner(&value) == OK); no_transfers(&value);
    CHECK(gp_pipeline_can_start(words(value.state, STATE_WORDS)));
    end(before);
    /* Keep the native state at its original address when reusing an owner. */
    begin();
    CHECK(start(&value) == OK); done = 0;
    CHECK(wait_owner(&value, &done) == OK && done == 1);
    CHECK(value.meta[4] == 23 && value.meta[10] == 42); no_transfers(&value); end(before);
}
static void descriptor_isolation(void) {
    int before = descriptors();
    int saved_flags = fcntl(199, F_GETFD);
    CHECK(saved_flags >= 0 || errno == EBADF);
    int saved = fcntl(199, F_DUPFD_CLOEXEC, 200);
    CHECK(saved >= 0 || errno == EBADF);
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC); CHECK(null_fd >= 0);
    CHECK(dup2(null_fd, 199) == 199);
    if (null_fd != 199) CHECK(close(null_fd) == 0);
    CHECK(fcntl(199, F_SETFD, 0) == 0); /* Deliberately inheritable in the parent. */
    int during = descriptors(); begin(); owner value; initialize(&value);
    CHECK(start(&value) == OK);
    for (int i = 0; i < STAGES; ++i) {
        char path[64];
        int length = snprintf(path, sizeof path, "/proc/%ld/fd/199", (long)fixture.pid[i]);
        CHECK(length > 0 && (size_t)length < sizeof path);
        CHECK(access(path, F_OK) == -1 && errno == ENOENT);
    }
    CHECK(close_owner(&value) == OK); killed(&value, true, true); end(during);
    if (saved >= 0) {
        CHECK(dup2(saved, 199) == 199); CHECK(fcntl(199, F_SETFD, saved_flags) == 0);
        CHECK(close(saved) == 0);
    }
    else CHECK(close(199) == 0);
    CHECK(descriptors() == before);
}
static void closed_standard_descriptors(void) {
    for (unsigned mask = 1; mask < 8; ++mask) {
        pid_t child = fork(); CHECK(child >= 0);
        if (child == 0) {
            for (int fd = 0; fd < 3; ++fd) if (mask & (1u << fd)) CHECK(close(fd) == 0);
            int before = descriptors(); begin(); owner value; initialize(&value);
            value.command[PRODUCER] = command(value.first_command, sizeof value.first_command,
                "source", "32", "7");
            value.command[CONSUMER] = command(value.second_command, sizeof value.second_command,
                "copy", "8", NULL);
            CHECK(start(&value) == OK);
            int64_t done = 0; CHECK(wait_owner(&value, &done) == OK && done == 1);
            CHECK(value.meta[4] == 0 && value.meta[10] == 0);
            CHECK(value.meta[5] == 0 && value.meta[11] == 0);
            CHECK(value.meta[7] == 7 && value.meta[12] == 32 && value.meta[13] == 8);
            for (int i = 0; i < 32; ++i) CHECK(value.out[i] == 'x');
            for (int i = 0; i < 7; ++i) CHECK(value.first_err[i] == 'y');
            for (int i = 0; i < 8; ++i) CHECK(value.second_err[i] == 'z');
            no_transfers(&value); CHECK(close_owner(&value) == OK); end(before); _exit(0);
        }
        int status; CHECK(terminal_wait(child, &status) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
static void descendant_capture(void) {
    /* This isolated test, rather than the library, owns orphan adoption. */
    int was_subreaper = 0;
    CHECK(prctl(PR_GET_CHILD_SUBREAPER, &was_subreaper) == 0);
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    int before = descriptors(); begin(); fixture.check_order = false;
    owner value; initialize(&value); value.first[0] = value.second[0] = 1000;
    value.command[PRODUCER] = command(value.first_command, sizeof value.first_command, "exit", "0", NULL);
    value.command[CONSUMER] = command(value.second_command, sizeof value.second_command, "descendant", NULL, NULL);
    CHECK(start(&value) == OK);
    int64_t done = 0; CHECK(wait_owner(&value, &done) == TIMEOUT && done == 1);
    CHECK(value.meta[4] == 0 && value.meta[10] == 0);
    CHECK(value.meta[5] == 0 && value.meta[11] == 0);
    CHECK(fixture.kill_requested == 0); no_transfers(&value);
    CHECK(value.meta[12] > 1 && value.meta[12] < (int64_t)sizeof value.out);
    value.out[value.meta[12]] = 0;
    char *tail; long number = strtol((const char *)value.out, &tail, 10);
    CHECK(number > 0 && (long)(pid_t)number == number && *tail == '\n' && !tail[1]);
    pid_t descendant = (pid_t)number;
    siginfo_t info = {0};
    CHECK(waitid(P_PID, (id_t)descendant, &info, WEXITED | WNOHANG | WNOWAIT) == 0);
    CHECK(info.si_pid == 0); /* Alive and owned, with no other waiter. */
    int pidfd = (int)__real_syscall(SYS_pidfd_open, descendant, 0); CHECK(pidfd >= 0);
    CHECK(__real_syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, NULL, 0) == 0);
    int status; CHECK(terminal_wait(descendant, &status) == descendant);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(close(pidfd) == 0);
    CHECK(close_owner(&value) == TIMEOUT); no_transfers(&value); end(before);
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, was_subreaper) == 0);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); helper = argv[1]; CHECK(helper[0] == '/');
    for (int i = 0; i < STAGES; ++i) fixture.recovery_pidfd[i] = -1;
    CHECK(atexit(emergency_cleanup) == 0);
    int before = descriptors();
    ordinary_close(); launch_failures(); postspawn_failure(); shared_deadline(); interrupted_deadline();
    denied_waits(); denied_signals(); validation(); exit_statuses_and_reuse();
    descriptor_isolation(); closed_standard_descriptors(); descendant_capture();
    CHECK(descriptors() == before); no_children();
    puts("pipeline ownership, fault, deadline, validation and descriptor tests passed");
    return 0;
}
