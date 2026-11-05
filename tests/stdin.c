/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool fail_after_send, clock_failed, inspect_idle;
static ssize_t accepted;
static int idle_polls;
static bool eagain_once;
static int send_calls;
static bool deny_reap_once;
static pid_t tested_pid;
static int wait_denials;
pid_t __real_waitpid(pid_t pid, int *status, int flags);
pid_t __wrap_waitpid(pid_t pid, int *status, int flags) {
    if (clock_failed && deny_reap_once && pid == tested_pid) {
        deny_reap_once = false; ++wait_denials; errno = EIO; return -1;
    }
    return __real_waitpid(pid, status, flags);
}
int __real_clock_gettime(clockid_t clock, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock, struct timespec *value) {
    if (clock_failed) { errno = EIO; return -1; }
    return __real_clock_gettime(clock, value);
}
ssize_t __real_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
    assert(flags & MSG_NOSIGNAL);
    ++send_calls;
    if (eagain_once) { eagain_once = false; errno = EAGAIN; return -1; }
    ssize_t count = __real_send(fd, data, size, flags);
    if (fail_after_send && count > 0) { accepted = count; clock_failed = true; }
    return count;
}
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __real___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes);
static int idle_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 4 && fds[3].fd >= 0 && fds[3].events == 0 && timeout == 0);
    ++idle_polls;
    return 0;
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    return inspect_idle ? idle_poll(fds, count, timeout) : __real_poll(fds, count, timeout);
}
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    return inspect_idle ? idle_poll(fds, count, timeout) : __real___poll_chk(fds, count, timeout, bytes);
}
static gp_metadata words(int64_t *data, int64_t length) { return (gp_metadata){data, length}; }
static gp_input empty(void) { return (gp_input){NULL, 0}; }
static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    int n = 0; while (readdir(directory)) ++n;
    assert(closedir(directory) == 0); return n;
}
int main(void) {
    static const uint8_t command[] = "/bin/cat\0";
    int64_t state[24] = {0}, meta[6], progress[3];
    int64_t config[11] = {5000, 3, 3, 4, -1, -1, -1, 0, 0, 0, 0};
    int before = descriptors();
    assert(gp_start((gp_input){command, sizeof command - 1}, empty(), empty(), empty(), 0,
                    words(config, 11), words(state, 24), words(meta, 6)) == 0);
    pid_t pid = (pid_t)gp_id(words(state, 24)); assert(pid > 0);
    gp_output none = {NULL, 0};
    inspect_idle = true;
    assert(gp_io_step(words(state, 24), empty(), none, none, 0, words(meta, 6), words(progress, 3)) == 0);
    inspect_idle = false;
    assert(idle_polls == 1 && !progress[0] && progress[1] == 0 && progress[2]);
    /* Result aliases must reject before any bytes or state are consumed. */
    uint8_t input[] = {'a', 0, 255};
    assert(gp_io_step(words(state, 24), empty(), none, none, 0,
                       words(meta, 6), words(meta, 3)) == 1);
    assert(gp_io_step(words(state, 24), (gp_input){input, sizeof input}, (gp_output){input, sizeof input}, none,
                       0, words(meta, 6), words(progress, 3)) == 1);
    assert(gp_io_step(words(state, 24), empty(), (gp_output){(uint8_t *)state, sizeof state}, none,
                       0, words(meta, 6), words(progress, 3)) == 1);
    assert(gp_io_step(words(state, 24), (gp_input){input, sizeof input}, none, none, 0,
                       words(meta, 6), words(state, 3)) == 1);
    assert(gp_io_step(words(state, 24), (gp_input){(uint8_t *)progress, sizeof progress}, none, none, 0,
                       words(meta, 6), words(progress, 3)) == 1);
    assert(gp_id(words(state, 24)) == pid);
    eagain_once = true;
    assert(gp_io_step(words(state, 24), (gp_input){input, sizeof input}, none, none, 20,
                       words(meta, 6), words(progress, 3)) == 0);
    assert(!progress[0] && progress[1] == 0 && progress[2] && send_calls == 1);
    assert(gp_io_step(words(state, 24), empty(), none, none, 0, words(meta, 6), words(progress, 3)) == 0);
    assert(progress[1] == 0 && progress[2] && send_calls == 1); /* No retained offer/queue. */
    fail_after_send = true;
    int code = (int)gp_io_step(words(state, 24), (gp_input){input, sizeof input}, none, none, 20,
                               words(meta, 6), words(progress, 3));
    clock_failed = false; fail_after_send = false;
    assert(code == 3 && meta[0] == EIO && meta[5] == 0);
    assert(accepted > 0 && progress[1] == accepted && progress[0] && !progress[2]);
    errno = 0; assert(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(gp_io_step(words(state, 24), empty(), none, none, 0, words(meta, 6), words(progress, 3)) == 3);
    assert(progress[0] && progress[1] == 0 && !progress[2]);
    assert(gp_close_stdin(words(state, 24), words(meta, 6)) == 3);
    assert(gp_close(words(state, 24), words(meta, 6)) == 3 && meta[5] == 0);
    /* A targeted denied terminal wait transfers this child exactly once while
       preserving accepted-byte accounting. The local recovery owner reaps it. */
    assert(gp_start((gp_input){command, sizeof command - 1}, empty(), empty(), empty(), 0,
                    words(config, 11), words(state, 24), words(meta, 6)) == 0);
    tested_pid = (pid_t)gp_id(words(state, 24)); assert(tested_pid > 0);
    deny_reap_once = true; fail_after_send = true; accepted = 0;
    code = (int)gp_io_step(words(state, 24), (gp_input){input, sizeof input}, none, none, 20,
                           words(meta, 6), words(progress, 3));
    clock_failed = false; fail_after_send = false;
    assert(code == 3 && wait_denials == 1 && meta[5] == tested_pid);
    assert(accepted > 0 && progress[1] == accepted && progress[0] && !progress[2]);
    assert(gp_close(words(state, 24), words(meta, 6)) == 3 && meta[5] == 0);
    int terminal; pid_t recovered;
    do { recovered = waitpid(tested_pid, &terminal, 0); } while (recovered < 0 && errno == EINTR);
    assert(recovered == tested_pid && (WIFSIGNALED(terminal) || WIFEXITED(terminal)));
    errno = 0; assert(waitpid(tested_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    /* Borrowed output routing still works while caller-driven input is pumped. */
    config[1] = 5; config[4] = open("/dev/null", O_WRONLY | O_CLOEXEC); assert(config[4] >= 0);
    assert(gp_start((gp_input){command, sizeof command - 1}, empty(), empty(), empty(), 0,
                    words(config, 11), words(state, 24), words(meta, 6)) == 0);
    assert(close((int)config[4]) == 0);
    assert(gp_io_step(words(state, 24), (gp_input){input, sizeof input}, none, none, 20,
                       words(meta, 6), words(progress, 3)) == 0 && progress[1] == sizeof input);
    assert(gp_close_stdin(words(state, 24), words(meta, 6)) == 0);
    do {
        assert(gp_io_step(words(state, 24), empty(), none, none, 20, words(meta, 6), words(progress, 3)) == 0);
    } while (!progress[0]);
    assert(meta[1] == 0 && !progress[2]);
    assert(descriptors() == before);
    puts("streaming stdin accepted-byte, idle-poll and cleanup fault tests passed");
    return 0;
}
