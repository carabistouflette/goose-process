/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "internal.h"
#if !defined(__linux__) || !defined(__GLIBC__)
#error "goose-process requires Linux and glibc 2.34+"
#elif !__GLIBC_PREREQ(2, 34)
#error "goose-process requires glibc 2.34+"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Copied into the noncopyable Goose owner's aligned i64 storage. No pointer
   into Goose storage or stdin payload is retained across calls. */
typedef struct {
    int64_t owner, phase, pid, pidfd, in_fd, out_fd, err_fd, deadline;
    int64_t input_size, input_sent, out_limit, err_limit, out_size, err_size;
    int64_t reaped, ready, result, os_error, exit_code, signal, transfer_pid;
    int64_t out_stream, err_stream, reserved;
} child;
_Static_assert(sizeof(child) == GP_CHILD_WORDS * sizeof(int64_t), "child ABI size");
_Static_assert(sizeof(gp_config) == GP_CONFIG_WORDS * sizeof(int64_t), "config ABI size");

static void close_fd(int *fd) {
    int saved = errno;
    if (*fd >= 0) close(*fd); /* Linux releases the descriptor even on EINTR. */
    *fd = -1; errno = saved;
}
static void close_slot(int64_t *slot) { int fd = (int)*slot; close_fd(&fd); *slot = -1; }
static int take(int *fd) { int result = *fd; *fd = -1; return result; }
static int promote(int fd) {
    if (fd < 0 || fd >= 3) return fd;
    int result = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close_fd(&fd);
    return result;
}
static int make_pipe(int fds[2], int nonblocking_end) {
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    fds[0] = promote(fds[0]);
    if (fds[0] < 0) { close_fd(&fds[1]); return -1; }
    fds[1] = promote(fds[1]);
    if (fds[1] < 0) { close_fd(&fds[0]); return -1; }
    int flags = fcntl(fds[nonblocking_end], F_GETFL);
    if (flags < 0 || fcntl(fds[nonblocking_end], F_SETFL, flags | O_NONBLOCK) < 0) {
        close_fd(&fds[0]); close_fd(&fds[1]); return -1;
    }
    return 0;
}
int64_t gp_clock_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    if (now.tv_sec < 0 || now.tv_sec > (INT64_MAX - now.tv_nsec) / 1000000000) {
        errno = EOVERFLOW; return -1;
    }
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
static bool overlaps(gp_output a, gp_output b) {
    if (a.len <= 0 || b.len <= 0) return false;
    uintptr_t x = (uintptr_t)a.data, y = (uintptr_t)b.data;
    return x <= y ? y - x < (uint64_t)a.len : x - y < (uint64_t)b.len;
}
static bool buffer(gp_output value) {
    return value.len >= 0 && value.len <= GP_OUTPUT && (value.len == 0 || value.data);
}
static gp_output words(gp_metadata value) {
    return (gp_output){(uint8_t *)value.data, value.len * (int64_t)sizeof(int64_t)};
}
static void empty_meta(gp_metadata meta) {
    meta.data[0] = 0; meta.data[1] = -1;
    for (int i = 2; i < 6; ++i) meta.data[i] = 0;
}
static int emit(child *value, gp_metadata meta) {
    meta.data[0] = value->os_error; meta.data[1] = value->exit_code;
    meta.data[2] = value->signal; meta.data[3] = value->out_size;
    meta.data[4] = value->err_size; meta.data[5] = value->transfer_pid;
    value->transfer_pid = 0; /* Transfer ownership exactly once. */
    return (int)value->result;
}
static bool load(gp_metadata storage, child *value) {
    if (storage.len != GP_CHILD_WORDS || !storage.data) return false;
    memcpy(value, storage.data, sizeof *value);
    return value->owner == (int64_t)(uintptr_t)storage.data &&
           (value->phase == 1 || value->phase == 2);
}
static void store(gp_metadata storage, const child *value) { memcpy(storage.data, value, sizeof *value); }
int64_t gp_can_start(gp_metadata storage) {
    if (storage.len != GP_CHILD_WORDS || !storage.data) return 0;
    bool empty = true;
    for (int i = 0; i < GP_CHILD_WORDS; ++i) if (storage.data[i]) empty = false;
    child value;
    return empty || (load(storage, &value) && value.phase == 2);
}
int64_t gp_id(gp_metadata storage) {
    child value;
    return load(storage, &value) && value.phase == 1 && !value.reaped ? value.pid : -1;
}
static bool record_status(child *value, int status) {
    if (WIFEXITED(status)) { value->exit_code = WEXITSTATUS(status); return true; }
    if (WIFSIGNALED(status)) { value->signal = WTERMSIG(status); return true; }
    return false;
}
static int reap_ready(child *value) {
    if (value->reaped) return 0;
    int status;
    pid_t waited = waitpid((pid_t)value->pid, &status, WNOHANG);
    if (waited == value->pid && record_status(value, status)) {
        value->reaped = 1; close_slot(&value->pidfd); close_slot(&value->in_fd);
    } else if (waited < 0 && errno != EINTR) {
        if (errno == ECHILD) { value->reaped = 1; close_slot(&value->pidfd); }
        return -1;
    }
    return 0;
}
static void finish(child *value) {
    close_slot(&value->in_fd); close_slot(&value->out_fd); close_slot(&value->err_fd);
    if (value->pid > 0 && !value->reaped) {
        int sent = -1;
        if (value->pidfd >= 0) {
            do { sent = (int)syscall(SYS_pidfd_send_signal, (int)value->pidfd, SIGKILL, NULL, 0); }
            while (sent < 0 && errno == EINTR);
        }
        if (sent < 0 && (value->pidfd < 0 || errno != ESRCH)) {
            do { sent = kill((pid_t)value->pid, SIGKILL); } while (sent < 0 && errno == EINTR);
        }
        bool denied = sent < 0 && errno != ESRCH;
        if (denied) { value->result = GP_SYSTEM; value->os_error = errno; }
        int status; pid_t waited;
        do { waited = waitpid((pid_t)value->pid, &status, denied ? WNOHANG : 0); }
        while ((waited < 0 && errno == EINTR) ||
               (waited == value->pid && !record_status(value, status) && !denied));
        if (waited == value->pid) {
            if (record_status(value, status)) value->reaped = 1;
            else value->transfer_pid = value->pid;
        } else if (waited == 0) value->transfer_pid = value->pid;
        else {
            value->result = GP_SYSTEM; value->os_error = errno;
            if (errno != ECHILD) value->transfer_pid = value->pid;
        }
    }
    close_slot(&value->pidfd);
    value->phase = 2;
}

static int make_input(int fds[2]);

static bool method_meta(gp_metadata storage, gp_metadata meta) {
    if (storage.len != GP_CHILD_WORDS || !storage.data || meta.len != 6 || !meta.data ||
        overlaps(words(storage), words(meta))) return false;
    return true;
}
static bool route(int64_t mode, bool stderr_stream) {
    return mode == GP_OUT_CAPTURE || mode == GP_OUT_INHERIT || mode == GP_OUT_NULL ||
           mode == GP_OUT_FD || mode == GP_OUT_STREAM || (stderr_stream && mode == GP_OUT_MERGE);
}
static bool source_fd(int64_t mode, int64_t fd, int64_t fd_mode) {
    return mode == fd_mode ? fd >= 0 && fd <= INT_MAX : fd == -1;
}

int64_t gp_start(gp_input packed, gp_input cwd, gp_input sets, gp_input removes,
    int64_t input_size, gp_metadata settings, gp_metadata storage, gp_metadata meta) {
    if (!method_meta(storage, meta)) return GP_INVALID;
    if (settings.len != GP_CONFIG_WORDS || !settings.data) return GP_INVALID;
    gp_output inputs[] = {{(uint8_t *)packed.data, packed.len}, {(uint8_t *)cwd.data, cwd.len},
        {(uint8_t *)sets.data, sets.len}, {(uint8_t *)removes.data, removes.len}, words(settings)};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i)
        if (overlaps(inputs[i], words(storage)) || overlaps(inputs[i], words(meta))) return GP_INVALID;
    empty_meta(meta);
    if (!gp_can_start(storage)) return GP_STATE;
    gp_config config; memcpy(&config, settings.data, sizeof config);
    if (config.timeout_ms < 0 || config.timeout_ms > 86400000 ||
        !route(config.out_mode, false) || !route(config.err_mode, true) ||
        config.in_mode < GP_IN_NULL || config.in_mode > GP_IN_FD ||
        !source_fd(config.in_mode, config.in_fd, GP_IN_FD) ||
        !source_fd(config.out_mode, config.out_fd, GP_OUT_FD) ||
        !source_fd(config.err_mode, config.err_fd, GP_OUT_FD) ||
        (config.clear_env != 0 && config.clear_env != 1) ||
        (config.search_path != 0 && config.search_path != 1) ||
        config.out_limit < 0 || config.out_limit > GP_OUTPUT ||
        config.err_limit < 0 || config.err_limit > GP_OUTPUT ||
        input_size < 0 || input_size > GP_INPUT ||
        (config.in_mode != GP_IN_BYTES && input_size != 0) ||
        cwd.len < 0 || cwd.len > GP_PATH || (cwd.len && !cwd.data) ||
        (cwd.len && memchr(cwd.data, 0, (size_t)cwd.len))) return GP_INVALID;
    child value = {0};
    value.owner = (int64_t)(uintptr_t)storage.data; value.phase = 1;
    value.pid = value.pidfd = value.in_fd = value.out_fd = value.err_fd = -1;
    value.exit_code = -1; value.result = GP_SYSTEM;
    value.input_size = input_size;
    value.out_limit = config.out_mode == GP_OUT_CAPTURE ? config.out_limit : 0;
    value.err_limit = config.err_mode == GP_OUT_CAPTURE ? config.err_limit : 0;
    value.out_stream = config.out_mode == GP_OUT_STREAM;
    value.err_stream = config.err_mode == GP_OUT_STREAM;
    gp_command command = {0};
    int snapshots[3] = {-1, -1, -1};
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    int null_read = -1, null_write = -1;
    bool actions_ready = false, attributes_ready = false;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int64_t sources[] = {config.in_fd, config.out_fd, config.err_fd};
    /* Check every original number before any internal allocation can reuse a
       closed source descriptor. Caller keeps source FDs stable through start. */
    for (int i = 0; i < 3; ++i)
        if (sources[i] >= 0 && fcntl((int)sources[i], F_GETFD) < 0) goto system_error;
    int preparation_error = 0;
    int result = gp_command_prepare(packed, sets, removes, &config, &command, &preparation_error);
    /* gp_command_prepare reports errno separately from classification. */
    if (result) {
        value.result = result;
        value.os_error = preparation_error;
        goto failed;
    }
    int64_t began = gp_clock_ns();
    if (began < 0) goto system_error;
    int64_t budget = config.timeout_ms * 1000000;
    if (began > INT64_MAX - budget) { errno = EOVERFLOW; goto system_error; }
    value.deadline = config.timeout_ms ? began + budget : 0;
    struct sigaction disposition;
    if (sigaction(SIGCHLD, NULL, &disposition) < 0) goto system_error;
    if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT)) {
        errno = EINVAL; goto system_error;
    }
    for (int i = 0; i < 3; ++i) {
        if (sources[i] < 0) continue;
        snapshots[i] = fcntl((int)sources[i], F_DUPFD_CLOEXEC, 3);
        if (snapshots[i] < 0) goto system_error;
    }
    if (config.in_mode == GP_IN_NULL) {
        null_read = promote(open("/dev/null", O_RDONLY | O_CLOEXEC));
        if (null_read < 0) goto system_error;
    }
    if (config.out_mode == GP_OUT_NULL || config.err_mode == GP_OUT_NULL) {
        null_write = promote(open("/dev/null", O_WRONLY | O_CLOEXEC));
        if (null_write < 0) goto system_error;
    }
    if (config.in_mode == GP_IN_BYTES) {
        if (make_input(in_pipe) < 0) goto system_error;
        value.in_fd = take(&in_pipe[1]);
        if (!input_size) close_slot(&value.in_fd);
    }
    if (config.out_mode == GP_OUT_CAPTURE || value.out_stream) {
        if (make_pipe(out_pipe, 0) < 0) goto system_error;
        value.out_fd = take(&out_pipe[0]);
    }
    if (config.err_mode == GP_OUT_CAPTURE || value.err_stream) {
        if (make_pipe(err_pipe, 0) < 0) goto system_error;
        value.err_fd = take(&err_pipe[0]);
    }
    int action_error = posix_spawn_file_actions_init(&actions);
    if (action_error) { value.os_error = action_error; goto failed; }
    actions_ready = true;
#define ACTION(call) do { action_error = (call); if (action_error) { value.os_error = action_error; goto failed; } } while (0)
    ACTION(posix_spawnattr_init(&attributes)); attributes_ready = true;
    sigset_t empty_mask, defaults;
    sigemptyset(&empty_mask); sigfillset(&defaults);
    ACTION(posix_spawnattr_setsigmask(&attributes, &empty_mask));
    ACTION(posix_spawnattr_setsigdefault(&attributes, &defaults));
    ACTION(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    int child_input = config.in_mode == GP_IN_NULL ? null_read :
                      config.in_mode == GP_IN_FD ? snapshots[0] : in_pipe[0];
    if (config.in_mode != GP_IN_INHERIT)
        ACTION(posix_spawn_file_actions_adddup2(&actions, child_input, STDIN_FILENO));
    int child_output = config.out_mode == GP_OUT_CAPTURE || value.out_stream ? out_pipe[1] :
                       config.out_mode == GP_OUT_NULL ? null_write : snapshots[1];
    if (config.out_mode != GP_OUT_INHERIT)
        ACTION(posix_spawn_file_actions_adddup2(&actions, child_output, STDOUT_FILENO));
    int child_error = config.err_mode == GP_OUT_CAPTURE || value.err_stream ? err_pipe[1] :
                      config.err_mode == GP_OUT_NULL ? null_write : snapshots[2];
    if (config.err_mode == GP_OUT_MERGE)
        ACTION(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO));
    else if (config.err_mode != GP_OUT_INHERIT)
        ACTION(posix_spawn_file_actions_adddup2(&actions, child_error, STDERR_FILENO));
    char directory[GP_PATH + 1];
    if (cwd.len) {
        memcpy(directory, cwd.data, (size_t)cwd.len); directory[cwd.len] = 0;
        ACTION(posix_spawn_file_actions_addchdir_np(&actions, directory));
    }
    ACTION(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#undef ACTION
    pid_t pid = -1;
    int launch_error = 0;
    value.result = gp_command_spawn(&command, &config, &actions, &attributes,
                                   value.deadline, &pid, &launch_error);
    value.os_error = launch_error;
    if (value.result) goto failed;
    value.pid = pid;
    close_fd(&null_read); close_fd(&null_write);
    for (int i = 0; i < 3; ++i) close_fd(&snapshots[i]);
    close_fd(&in_pipe[0]); close_fd(&out_pipe[1]); close_fd(&err_pipe[1]);
    value.pidfd = promote((int)syscall(SYS_pidfd_open, pid, 0));
    if (value.pidfd < 0) goto system_error;
    goto cleanup;
system_error:
    value.result = GP_SYSTEM; value.os_error = errno;
failed:
    finish(&value);
cleanup:
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    for (int i = 0; i < 3; ++i) close_fd(&snapshots[i]);
    close_fd(&null_read); close_fd(&null_write);
    close_fd(&in_pipe[0]); close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
    close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
    gp_command_destroy(&command);
    int code = emit(&value, meta); store(storage, &value);
    return code;
}

/* Finite input uses a one-way local socket, so MSG_NOSIGNAL can suppress
   write-side SIGPIPE without touching caller masks, handlers or pending state.
   Input.Fd remains available when the child requires a real FIFO descriptor. */
static int make_input(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) return -1;
    fds[0] = promote(fds[0]);
    if (fds[0] < 0) { close_fd(&fds[1]); return -1; }
    fds[1] = promote(fds[1]);
    if (fds[1] < 0) { close_fd(&fds[0]); return -1; }
    int flags = fcntl(fds[1], F_GETFL);
    if (flags < 0 || fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) < 0 ||
        shutdown(fds[0], SHUT_WR) < 0 || shutdown(fds[1], SHUT_RD) < 0) {
        close_fd(&fds[0]); close_fd(&fds[1]); return -1;
    }
    return 0;
}

static int drain(int64_t *fd, gp_output target, int64_t *length, bool stream) {
    uint8_t block[16384];
    int64_t remaining = target.len - *length;
    if (stream && remaining == 0) return GP_OK;
    size_t capacity = stream && remaining < (int64_t)sizeof block ? (size_t)remaining : sizeof block;
    ssize_t count = read((int)*fd, stream ? target.data + *length : block, capacity);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return GP_OK;
        return GP_SYSTEM;
    }
    if (count == 0) { close_slot(fd); return GP_OK; }
    int64_t take_count = count < remaining ? count : remaining;
    if (take_count && !stream) memcpy(target.data + *length, block, (size_t)take_count);
    *length += take_count;
    return count > remaining ? GP_LIMIT : GP_OK;
}

static void pump(child *value, gp_input input, gp_output out, gp_output err,
                 int64_t slice_ms, bool one_step) {
    if (!value->out_stream) out.len = value->out_limit;
    if (!value->err_stream) err.len = value->err_limit;
    int64_t slice_deadline = 0;
    bool polled = false;
    if (one_step) {
        int64_t began = gp_clock_ns();
        if (began < 0) goto system_error;
        if (began > INT64_MAX - slice_ms * 1000000) { errno = EOVERFLOW; goto system_error; }
        slice_deadline = began + slice_ms * 1000000;
    }
    for (;;) {
        if (!value->reaped && value->ready && reap_ready(value) < 0) goto system_error;
        if (value->reaped && value->out_fd < 0 && value->err_fd < 0) {
            finish(value); return;
        }
        int64_t now = gp_clock_ns();
        if (now < 0) goto system_error;
        if (value->deadline && now >= value->deadline) { value->result = GP_TIMEOUT; finish(value); return; }
        if (one_step && polled && now >= slice_deadline) return;
        int64_t remaining = value->deadline ? value->deadline - now : INT64_MAX;
        if (one_step && slice_deadline - now < remaining)
            remaining = slice_deadline > now ? slice_deadline - now : 0;
        int64_t wait_ms = remaining / 1000000 + (remaining % 1000000 != 0);
        struct pollfd fds[4] = {
            {value->out_stream && !out.len ? -1 : (int)value->out_fd, POLLIN, 0},
            {value->err_stream && !err.len ? -1 : (int)value->err_fd, POLLIN, 0},
            {(int)value->pidfd, POLLIN, 0}, {(int)value->in_fd, POLLOUT, 0}
        };
        polled = true;
        int timeout = !one_step && !value->deadline ? -1 : wait_ms > INT_MAX ? INT_MAX : (int)wait_ms;
        int ready = poll(fds, 4, timeout);
        if (ready < 0) { if (errno == EINTR) continue; goto system_error; }
        for (int i = 0; i < 4; ++i)
            if (fds[i].revents & POLLNVAL) { errno = EBADF; goto system_error; }
        value->ready = (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
        for (int i = 0; i < 2; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int result = drain(i ? &value->err_fd : &value->out_fd,
                               i ? err : out, i ? &value->err_size : &value->out_size,
                               i ? value->err_stream : value->out_stream);
            if (result == GP_SYSTEM) goto system_error;
            if (result == GP_LIMIT) { value->result = GP_LIMIT; finish(value); return; }
        }
        if (fds[3].revents & (POLLERR | POLLHUP)) close_slot(&value->in_fd);
        else if (fds[3].revents & POLLOUT) {
            int64_t count = value->input_size - value->input_sent;
            if (count > 16384) count = 16384;
            ssize_t written = send((int)value->in_fd, input.data + value->input_sent, (size_t)count, MSG_NOSIGNAL);
            if (written > 0) {
                value->input_sent += written;
                if (value->input_sent == value->input_size) close_slot(&value->in_fd);
            } else if (written < 0 && errno == EPIPE) close_slot(&value->in_fd);
            else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) goto system_error;
        }
        if (!one_step) continue;
        if (value->ready && reap_ready(value) < 0) goto system_error;
        if (value->reaped && value->out_fd < 0 && value->err_fd < 0) { finish(value); return; }
        now = gp_clock_ns();
        if (now < 0) goto system_error;
        if (value->deadline && now >= value->deadline) { value->result = GP_TIMEOUT; finish(value); }
        return;
    }
system_error:
    value->result = GP_SYSTEM; value->os_error = errno; finish(value);
}

static int wait_impl(gp_metadata storage, gp_input input, gp_output out, gp_output err,
    int64_t slice_ms, int64_t one_step, gp_metadata meta, int64_t *done, bool streaming) {
    if (!method_meta(storage, meta) || !done || !buffer(out) || !buffer(err) ||
        input.len < 0 || input.len > GP_INPUT || (input.len && !input.data) ||
        slice_ms < 0 || slice_ms > 86400000 || (one_step != 0 && one_step != 1)) return GP_INVALID;
    gp_output payload = {(uint8_t *)input.data, input.len};
    gp_output finished = {(uint8_t *)done, sizeof *done};
    if (overlaps(out, err) || overlaps(out, words(storage)) || overlaps(err, words(storage)) ||
        overlaps(out, words(meta)) || overlaps(err, words(meta)) ||
        overlaps(payload, out) || overlaps(payload, err) || overlaps(payload, words(meta)) ||
        overlaps(payload, words(storage)) || overlaps(finished, words(storage)) ||
        overlaps(finished, words(meta)) || overlaps(finished, out) || overlaps(finished, err) ||
        overlaps(finished, payload)) return GP_INVALID;
    empty_meta(meta);
    child value;
    if (!load(storage, &value)) return GP_STATE;
    bool has_stream = value.out_stream || value.err_stream;
    if ((streaming && !has_stream) || (!streaming && has_stream && value.phase == 1)) return GP_STATE;
    if (value.phase == 1 && (input.len != value.input_size || out.len < value.out_limit || err.len < value.err_limit))
        return GP_INVALID;
    if (streaming) {
        if (value.out_stream) value.out_size = 0;
        if (value.err_stream) value.err_size = 0;
    }
    if (value.phase == 1) pump(&value, input, out, err, slice_ms, one_step != 0);
    *done = value.phase == 2;
    int result = emit(&value, meta); store(storage, &value);
    return result;
}

int64_t gp_wait(gp_metadata storage, gp_input input, gp_output out, gp_output err,
    int64_t slice_ms, int64_t one_step, gp_metadata meta, int64_t *done) {
    return wait_impl(storage, input, out, err, slice_ms, one_step, meta, done, false);
}

int64_t gp_stream_step(gp_metadata storage, gp_input input, gp_output out, gp_output err,
    int64_t slice_ms, gp_metadata meta, int64_t *done) {
    return wait_impl(storage, input, out, err, slice_ms, 1, meta, done, true);
}

int64_t gp_try_wait(gp_metadata storage, gp_metadata meta, int64_t *done) {
    if (!method_meta(storage, meta) || !done ||
        overlaps((gp_output){(uint8_t *)done, sizeof *done}, words(storage)) ||
        overlaps((gp_output){(uint8_t *)done, sizeof *done}, words(meta))) return GP_INVALID;
    empty_meta(meta);
    child value;
    if (!load(storage, &value)) return GP_STATE;
    if (value.phase == 1 && !value.reaped && reap_ready(&value) < 0) {
        int error = errno;
        if (value.reaped) { value.result = GP_SYSTEM; value.os_error = error; finish(&value); }
        else {
            int result = emit(&value, meta); (void)result;
            meta.data[0] = error;
            store(storage, &value); *done = 0;
            return GP_SYSTEM;
        }
    }
    *done = value.reaped;
    int result = emit(&value, meta); store(storage, &value);
    return result;
}

int64_t gp_signal(gp_metadata storage, int64_t signal_number, gp_metadata meta) {
    if (!method_meta(storage, meta)) return GP_INVALID;
    empty_meta(meta);
    if (signal_number < 0 || signal_number >= NSIG) return GP_INVALID;
    child value;
    if (!load(storage, &value) || value.phase != 1 || value.reaped) return GP_STATE;
    int result;
    do { result = (int)syscall(SYS_pidfd_send_signal, (int)value.pidfd, (int)signal_number, NULL, 0); }
    while (result < 0 && errno == EINTR);
    if (result < 0) { meta.data[0] = errno; return GP_SYSTEM; }
    return emit(&value, meta);
}

int64_t gp_close(gp_metadata storage, gp_metadata meta) {
    if (!method_meta(storage, meta)) return GP_INVALID;
    empty_meta(meta);
    child value;
    if (!load(storage, &value)) return gp_can_start(storage) ? GP_OK : GP_STATE;
    if (value.phase == 1) finish(&value);
    int result = emit(&value, meta); store(storage, &value);
    return result;
}

int64_t gp_execute(gp_input packed, gp_input cwd, gp_input sets, gp_input removes,
    gp_input input, gp_metadata settings, gp_output out, gp_output err, gp_metadata meta) {
    if (meta.len != 6 || !meta.data || settings.len != GP_CONFIG_WORDS || !settings.data) return GP_INVALID;
    gp_output inputs[] = {{(uint8_t *)packed.data, packed.len}, {(uint8_t *)cwd.data, cwd.len},
        {(uint8_t *)sets.data, sets.len}, {(uint8_t *)removes.data, removes.len},
        {(uint8_t *)input.data, input.len}, words(settings)};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i)
        if (overlaps(inputs[i], words(meta))) return GP_INVALID;
    if (overlaps(out, words(meta)) || overlaps(err, words(meta))) return GP_INVALID;
    empty_meta(meta);
    if (!buffer(out) || !buffer(err) || overlaps(out, err) || input.len < 0 ||
        input.len > GP_INPUT || (input.len && !input.data) ||
        overlaps((gp_output){(uint8_t *)input.data, input.len}, out) ||
        overlaps((gp_output){(uint8_t *)input.data, input.len}, err)) return GP_INVALID;
    gp_config config; memcpy(&config, settings.data, sizeof config);
    if (config.timeout_ms < 1 || config.out_mode == GP_OUT_STREAM || config.err_mode == GP_OUT_STREAM)
        return GP_INVALID;
    if ((config.out_mode == GP_OUT_CAPTURE && out.len < config.out_limit) ||
        (config.err_mode == GP_OUT_CAPTURE && err.len < config.err_limit)) return GP_INVALID;
    int64_t state[GP_CHILD_WORDS] = {0};
    gp_metadata storage = {state, GP_CHILD_WORDS};
    int result = (int)gp_start(packed, cwd, sets, removes, input.len, settings, storage, meta);
    if (result) return result;
    int64_t done;
    return gp_wait(storage, input, out, err, 0, 0, meta, &done);
}

int64_t gp_run(gp_input packed, gp_input cwd, int64_t timeout_ms,
               int64_t capture, gp_output out, gp_output err, gp_metadata meta) {
    int64_t mode = capture == 0 ? GP_OUT_INHERIT : capture == 1 ? GP_OUT_CAPTURE : GP_OUT_DEFAULT;
    int64_t settings[GP_CONFIG_WORDS] = {timeout_ms,
        mode, mode, GP_IN_NULL, -1, -1, -1,
        0, 0, out.len, err.len};
    return gp_execute(packed, cwd, (gp_input){NULL, 0}, (gp_input){NULL, 0},
        (gp_input){NULL, 0}, (gp_metadata){settings, GP_CONFIG_WORDS}, out, err, meta);
}
