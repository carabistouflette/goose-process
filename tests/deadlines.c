/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Synthetic monotonic times make deadline checks independent of scheduling. */
typedef struct {
    const char *name;
    int64_t timeout_ms;
    struct timespec clocks[4];
    int fail_clock;
    bool timeout_after_poll, interrupt_first_poll, needs_wide_clock;
    int result, os_error, spawns, polls, poll_timeout;
} deadline_case;

static const deadline_case *current;
static int clock_calls, poll_calls, spawn_calls;
static pid_t spawned_pid;

int __real_clock_gettime(clockid_t clock, struct timespec *value);
int __real_posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[]);

int __wrap_clock_gettime(clockid_t clock, struct timespec *value) {
    if (!current || clock != CLOCK_MONOTONIC) return __real_clock_gettime(clock, value);
    assert(clock_calls < 4);
    ++clock_calls;
    if (clock_calls == current->fail_clock) { errno = EIO; return -1; }
    *value = current->clocks[clock_calls - 1];
    return 0;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(current && fds && count == 3);
    assert(timeout == current->poll_timeout);
    ++poll_calls;
    if (current->interrupt_first_poll && poll_calls == 1) { errno = EINTR; return -1; }
    if (current->timeout_after_poll) return 0;
    errno = EIO;
    return -1;
}

int __wrap_posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[]) {
    ++spawn_calls;
    int result = __real_posix_spawn(pid, path, actions, attributes, argv, environment);
    if (!result) spawned_pid = *pid;
    return result;
}

static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    int count = 0;
    while (readdir(directory)) ++count;
    assert(closedir(directory) == 0);
    return count;
}

/* Recover a leaked fixture before asserting, so a regression leaves no child. */
static void assert_reaped(void) {
    int status;
    if (spawned_pid > 0) {
        pid_t waited = waitpid(spawned_pid, &status, WNOHANG);
        bool leaked = waited >= 0;
        if (waited == 0) {
            assert(kill(spawned_pid, SIGKILL) == 0);
            do { waited = waitpid(spawned_pid, &status, 0); } while (waited < 0 && errno == EINTR);
            assert(waited == spawned_pid);
        }
        assert(!leaked && waited == -1 && errno == ECHILD);
    }
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

int main(void) {
    static const deadline_case cases[] = {
        { .name = "one nanosecond elapsed retains one millisecond poll",
          .timeout_ms = 1, .clocks = {{1000, 999999999}, {1001, 0}, {1001, 0}},
          .result = 3, .os_error = EIO, .spawns = 1, .polls = 1, .poll_timeout = 1 },
        { .name = "fractional remaining milliseconds round upward",
          .timeout_ms = 3, .clocks = {{1000, 999999999}, {1001, 0}, {1001, 1000000}},
          .result = 3, .os_error = EIO, .spawns = 1, .polls = 1, .poll_timeout = 2 },
        { .name = "expired preparation never launches",
          .timeout_ms = 1, .clocks = {{1000, 999999999}, {1001, 999999}},
          .result = 4 },
        { .name = "initial clock failure",
          .timeout_ms = 1, .fail_clock = 1, .result = 3, .os_error = EIO },
        { .name = "prelaunch clock failure",
          .timeout_ms = 1, .clocks = {{1000, 0}}, .fail_clock = 2,
          .result = 3, .os_error = EIO },
        { .name = "postlaunch clock failure reaps",
          .timeout_ms = 1, .clocks = {{1000, 0}, {1000, 0}}, .fail_clock = 3,
          .result = 3, .os_error = EIO, .spawns = 1 },
        { .name = "clock seconds conversion overflow",
          .timeout_ms = 1, .clocks = {{(time_t)9223372037LL, 0}}, .needs_wide_clock = true,
          .result = 3, .os_error = EOVERFLOW },
        { .name = "clock nanosecond conversion overflow",
          .timeout_ms = 1, .clocks = {{(time_t)9223372036LL, 854775808}}, .needs_wide_clock = true,
          .result = 3, .os_error = EOVERFLOW },
        { .name = "deadline addition overflow",
          .timeout_ms = 1, .clocks = {{(time_t)9223372036LL, 854775807}}, .needs_wide_clock = true,
          .result = 3, .os_error = EOVERFLOW },
        { .name = "deadline exactly at INT64_MAX remains valid",
          .timeout_ms = 1,
          .clocks = {{(time_t)9223372036LL, 853775807}, {(time_t)9223372036LL, 853775807},
                     {(time_t)9223372036LL, 853775808}}, .needs_wide_clock = true,
          .result = 3, .os_error = EIO, .spawns = 1, .polls = 1, .poll_timeout = 1 },
        { .name = "postlaunch conversion overflow reaps",
          .timeout_ms = 1, .clocks = {{1000, 0}, {1000, 0}, {(time_t)9223372037LL, 0}},
          .needs_wide_clock = true, .result = 3, .os_error = EOVERFLOW, .spawns = 1 },
        { .name = "poll timeout expires the unchanged deadline",
          .timeout_ms = 1,
          .clocks = {{1000, 999999999}, {1001, 0}, {1001, 0}, {1001, 999999}},
          .timeout_after_poll = true, .result = 4, .spawns = 1, .polls = 1, .poll_timeout = 1 },
        { .name = "interrupted poll retains the same deadline",
          .timeout_ms = 1,
          .clocks = {{1000, 999999999}, {1001, 0}, {1001, 0}, {1001, 0}},
          .interrupt_first_poll = true, .result = 3, .os_error = EIO,
          .spawns = 1, .polls = 2, .poll_timeout = 1 }
    };
    uint8_t packed[] = "/bin/sleep\0" "60\0";
    size_t completed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const deadline_case *test = &cases[i];
        if (test->needs_wide_clock && sizeof(time_t) < 8) continue;
        int before = descriptors();
        clock_calls = 0; poll_calls = 0; spawn_calls = 0; spawned_pid = 0;
        int64_t meta[6];
        current = test;
        int result = (int)gp_run((gp_input){packed, sizeof packed - 1}, (gp_input){NULL, 0},
            test->timeout_ms, 1, (gp_output){NULL, 0}, (gp_output){NULL, 0}, (gp_metadata){meta, 6});
        current = NULL;
        assert_reaped();
        assert(result == test->result && meta[0] == test->os_error);
        assert(spawn_calls == test->spawns && poll_calls == test->polls);
        assert(meta[1] == -1 && meta[2] == (test->spawns ? SIGKILL : 0));
        assert(meta[3] == 0 && meta[4] == 0 && meta[5] == 0);
        assert(descriptors() == before);
        ++completed;
    }
    printf("%zu deterministic deadline cases passed\n", completed);
    return 0;
}
