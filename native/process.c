/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "process.h"
#if !defined(__linux__) || !defined(__GLIBC__) || !__GLIBC_PREREQ(2, 34)
#error "goose-process requires Linux and glibc 2.34+"
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
enum { GP_OK, GP_INVALID, GP_LAUNCH, GP_SYSTEM, GP_TIMEOUT, GP_LIMIT };
enum { GP_ARG_BYTES = 65536, GP_ARGS = 256, GP_ARG = 16384,
       GP_PATH = 4095, GP_OUTPUT = 1048576 };

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd); /* Linux releases the fd even on EINTR. */
    *fd = -1;
}

/* Move private descriptors out of the child stdio range before spawn actions. */
static int promote(int fd) {
    if (fd < 0 || fd >= 3) return fd;
    int next = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    int saved = errno;
    close(fd);
    errno = saved;
    return next;
}

static int make_pipe(int fds[2]) {
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    fds[0] = promote(fds[0]);
    if (fds[0] < 0) { close_fd(&fds[1]); return -1; }
    fds[1] = promote(fds[1]);
    if (fds[1] < 0) { close_fd(&fds[0]); return -1; }
    int flags = fcntl(fds[0], F_GETFL);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved = errno;
        close_fd(&fds[0]); close_fd(&fds[1]); errno = saved;
        return -1;
    }
    return 0;
}

static int64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool overlaps(gp_output a, gp_output b) {
    if (a.len <= 0 || b.len <= 0) return false;
    uintptr_t x = (uintptr_t)a.data, y = (uintptr_t)b.data;
    return x <= y ? y - x < (uint64_t)a.len : x - y < (uint64_t)b.len;
}

/* waitpid also reports ptrace stops without WUNTRACED. Only these states reap. */
static bool terminal_status(int status, gp_metadata meta) {
    if (WIFEXITED(status)) { meta.data[1] = WEXITSTATUS(status); return true; }
    if (WIFSIGNALED(status)) { meta.data[2] = WTERMSIG(status); return true; }
    return false;
}

/* One bounded read per stream per iteration: neither stream starves deadlines. */
static int drain(int *fd, gp_output target, int64_t *length) {
    uint8_t block[16384];
    ssize_t n = read(*fd, block, sizeof block);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return GP_OK;
        return GP_SYSTEM;
    }
    if (n == 0) { close_fd(fd); return GP_OK; }
    int64_t remaining = target.len - *length;
    int64_t take = n < remaining ? n : remaining;
    if (take) memcpy(target.data + *length, block, (size_t)take);
    *length += take;
    return n > remaining ? GP_LIMIT : GP_OK;
}

int64_t gp_run(gp_input packed, gp_input cwd, int64_t timeout_ms,
               int64_t capture, gp_output out, gp_output err, gp_metadata meta) {
    if (meta.len != 6 || !meta.data) return GP_INVALID;
    gp_output metadata = {(uint8_t *)meta.data, 6 * (int64_t)sizeof(int64_t)};
    if (overlaps(metadata, out) || overlaps(metadata, err) ||
        overlaps(metadata, (gp_output){(uint8_t *)packed.data, packed.len}) ||
        overlaps(metadata, (gp_output){(uint8_t *)cwd.data, cwd.len})) return GP_INVALID;
    meta.data[0] = 0; meta.data[1] = -1; meta.data[2] = 0;
    meta.data[3] = 0; meta.data[4] = 0; meta.data[5] = 0;
    if (!packed.data || packed.len < 2 || packed.len > GP_ARG_BYTES ||
        packed.data[0] != '/' || packed.data[packed.len - 1] != 0 ||
        cwd.len < 0 || cwd.len > GP_PATH || (cwd.len && !cwd.data) ||
        timeout_ms < 1 || timeout_ms > 86400000 || (capture != 0 && capture != 1) ||
        out.len < 0 || out.len > GP_OUTPUT || err.len < 0 || err.len > GP_OUTPUT ||
        (out.len && !out.data) || (err.len && !err.data) || overlaps(out, err) ||
        (cwd.len && memchr(cwd.data, 0, (size_t)cwd.len))) return GP_INVALID;
    size_t count = 0, start = 0;
    for (size_t i = 0; i < (size_t)packed.len; ++i) {
        if (!packed.data[i]) {
            size_t length = i - start;
            if (length > GP_ARG || (count == 0 && length > GP_PATH) || ++count > GP_ARGS)
                return GP_INVALID;
            start = i + 1;
        }
    }
    char *text = malloc((size_t)packed.len);
    if (!text) { meta.data[0] = ENOMEM; return GP_SYSTEM; }
    memcpy(text, packed.data, (size_t)packed.len);
    char *argv[GP_ARGS + 1];
    argv[0] = text;
    size_t k = 1;
    for (int64_t i = 0; i < packed.len - 1; ++i)
        if (!text[i]) argv[k++] = text + i + 1;
    argv[k] = NULL;
    char dir[GP_PATH + 1];
    if (cwd.len) memcpy(dir, cwd.data, (size_t)cwd.len);
    dir[cwd.len] = 0;
    int result = GP_SYSTEM, saved = 0;
    int pipes[2][2] = {{-1, -1}, {-1, -1}}, nullfd = -1, pidfd = -1;
    pid_t pid = -1;
    bool reaped = false, actions_ready = false, attributes_ready = false;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int64_t began = milliseconds();
    if (began < 0) goto system_error;
    int64_t deadline = began + timeout_ms;
    struct sigaction disposition;
    if (sigaction(SIGCHLD, NULL, &disposition) < 0) goto system_error;
    if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT)) {
        errno = EINVAL; goto system_error;
    }
    nullfd = promote(open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (nullfd < 0) goto system_error;
    if (capture && (make_pipe(pipes[0]) < 0 || make_pipe(pipes[1]) < 0)) goto system_error;
    saved = posix_spawn_file_actions_init(&actions);
    if (saved) goto cleanup;
    actions_ready = true;
#define ACTION(call) do { saved = (call); if (saved) goto cleanup; } while (0)
    ACTION(posix_spawnattr_init(&attributes));
    attributes_ready = true;
    sigset_t empty_mask, defaults;
    sigemptyset(&empty_mask); sigfillset(&defaults);
    ACTION(posix_spawnattr_setsigmask(&attributes, &empty_mask));
    ACTION(posix_spawnattr_setsigdefault(&attributes, &defaults));
    ACTION(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    ACTION(posix_spawn_file_actions_adddup2(&actions, nullfd, STDIN_FILENO));
    if (capture) {
        ACTION(posix_spawn_file_actions_adddup2(&actions, pipes[0][1], STDOUT_FILENO));
        ACTION(posix_spawn_file_actions_adddup2(&actions, pipes[1][1], STDERR_FILENO));
    }
    if (cwd.len) ACTION(posix_spawn_file_actions_addchdir_np(&actions, dir));
    ACTION(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#undef ACTION
    saved = posix_spawn(&pid, argv[0], &actions, &attributes, argv, environ);
    if (saved) { pid = -1; result = GP_LAUNCH; goto cleanup; }
    close_fd(&nullfd);
    close_fd(&pipes[0][1]); close_fd(&pipes[1][1]);
    /* Kernel >=5.3: poll child completion without a periodic wait/sleep tax. */
    pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) goto system_error;
    bool child_ready = false;
    for (;;) {
        int status;
        /* Pipe readability is independent of child exit. Avoid a wait syscall
           on every capture chunk; pidfd readiness tells us when to reap. */
        if (!reaped && child_ready) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid && terminal_status(status, meta)) {
                reaped = true;
                close_fd(&pidfd);
            } else if (waited < 0 && errno != EINTR) {
                /* ECHILD means the PID is no longer ours to signal. */
                if (errno == ECHILD) reaped = true;
                goto system_error;
            }
        }
        if (reaped && pipes[0][0] < 0 && pipes[1][0] < 0) { result = GP_OK; break; }
        int64_t now = milliseconds();
        if (now < 0) goto system_error;
        if (now >= deadline) { result = GP_TIMEOUT; break; }
        struct pollfd fds[3] = {
            {pipes[0][0], POLLIN, 0}, {pipes[1][0], POLLIN, 0}, {pidfd, POLLIN, 0}
        };
        int ready = poll(fds, 3, (int)(deadline - now));
        if (ready < 0) { if (errno == EINTR) continue; goto system_error; }
        for (int i = 0; i < 3; ++i) {
            if (fds[i].revents & POLLNVAL) { errno = EBADF; goto system_error; }
        }
        child_ready = (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
        for (int i = 0; i < 2; ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                int drained = drain(&pipes[i][0], i ? err : out, &meta.data[3 + i]);
                if (drained == GP_SYSTEM) goto system_error;
                if (drained == GP_LIMIT) { result = GP_LIMIT; goto cleanup; }
            }
        }
    }
    goto cleanup;
system_error:
    saved = errno;
    result = GP_SYSTEM;
cleanup:
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    for (int i = 0; i < 2; ++i) { close_fd(&pipes[i][0]); close_fd(&pipes[i][1]); }
    close_fd(&nullfd);
    if (pid > 0 && !reaped) {
        /* Never signal a reaped PID. No process-global signal state is changed. */
        int sent = -1;
        if (pidfd >= 0) {
            do { sent = (int)syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, NULL, 0); }
            while (sent < 0 && errno == EINTR);
        }
        /* Some sandboxes permit pidfds but not pidfd_send_signal. The host must
           not reap our child, so its PID remains reserved for this fallback. */
        if (sent < 0 && (pidfd < 0 || errno != ESRCH)) {
            do { sent = kill(pid, SIGKILL); } while (sent < 0 && errno == EINTR);
        }
        bool denied = sent < 0 && errno != ESRCH;
        if (denied) {
            saved = errno; result = GP_SYSTEM;
        }
        int status;
        pid_t waited;
        do { waited = waitpid(pid, &status, denied ? WNOHANG : 0); }
        while ((waited < 0 && errno == EINTR) ||
               (waited == pid && !terminal_status(status, meta) && !denied));
        if (waited == pid) {
            if (!terminal_status(status, meta)) meta.data[5] = pid;
        } else if (waited == 0) {
            /* Cancellation was denied: explicitly transfer this child to caller. */
            meta.data[5] = pid;
        } else {
            saved = errno; result = GP_SYSTEM;
            if (errno != ECHILD) meta.data[5] = pid;
        }
    }
    close_fd(&pidfd);
    free(text);
    meta.data[0] = saved;
    return result;
}
