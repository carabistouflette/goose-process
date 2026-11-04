/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/internal.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Link with --wrap=socketpair,--wrap=shutdown,--wrap=send. The child modes
   exercise the public execution boundary without depending on shell parsing. */
static int fault, fault_hit, shutdown_calls;
int __real_socketpair(int domain, int type, int protocol, int fds[2]);
int __wrap_socketpair(int domain, int type, int protocol, int fds[2]) {
    if (fault == 1) { ++fault_hit; errno = EIO; return -1; }
    return __real_socketpair(domain, type, protocol, fds);
}
int __real_shutdown(int fd, int how);
int __wrap_shutdown(int fd, int how) {
    ++shutdown_calls;
    if (fault == shutdown_calls + 1) { ++fault_hit; errno = EIO; return -1; }
    return __real_shutdown(fd, how);
}
ssize_t __real_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
    assert(flags & MSG_NOSIGNAL);
    if (fault == 4) { ++fault_hit; errno = EPERM; return -1; }
    return __real_send(fd, data, size, flags);
}

static char executable[PATH_MAX];
static uint8_t input_bytes[GP_INPUT], output_bytes[GP_OUTPUT], error_bytes[GP_OUTPUT];
static const char keep_key[] = "GOOSE_PROCESS_TEST_KEEP";
static const char remove_key[] = "GOOSE_PROCESS_TEST_REMOVE";
static const char empty_key[] = "GOOSE_PROCESS_TEST_EMPTY";
extern char **environ;

enum { TIMEOUT, OUT_MODE, ERR_MODE, IN_MODE, OUT_FD, ERR_FD, IN_FD,
       CLEAR_ENV, SEARCH_PATH, OUT_LIMIT, ERR_LIMIT };
static gp_input bytes(const void *data, size_t size) { return (gp_input){data, (int64_t)size}; }
static gp_input text(const char *value) { return bytes(value, strlen(value)); }
static void defaults(int64_t config[GP_CONFIG_WORDS]) {
    const int64_t initial[GP_CONFIG_WORDS] = {5000, GP_OUT_CAPTURE, GP_OUT_CAPTURE,
        GP_IN_NULL, -1, -1, -1, 0, 0, GP_OUTPUT, 64};
    memcpy(config, initial, sizeof initial);
}
static int descriptors(void) {
    int count = 0;
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    while (readdir(directory)) ++count;
    assert(closedir(directory) == 0);
    return count;
}
static void emit(int fd, const void *data, size_t size) {
    const uint8_t *position = data;
    while (size) {
        ssize_t count = write(fd, position, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) _exit(90);
        position += count; size -= (size_t)count;
    }
}
static void child_mode(const char *mode) {
    if (!strcmp(mode, "--exit")) _exit(0);
    if (!strcmp(mode, "--streams")) {
        emit(STDOUT_FILENO, "OUT", 3); emit(STDERR_FILENO, "ERR", 3); _exit(0);
    }
    if (!strcmp(mode, "--environment")) {
        for (char **entry = environ; *entry; ++entry) {
            emit(STDOUT_FILENO, *entry, strlen(*entry)); emit(STDOUT_FILENO, "\n", 1);
        }
        _exit(0);
    }
    if (!strcmp(mode, "--selected-environment")) {
        const char *keys[] = {keep_key, remove_key, empty_key};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
            const char *value = getenv(keys[i]);
            if (!value) value = "(unset)";
            emit(STDOUT_FILENO, value, strlen(value)); emit(STDOUT_FILENO, "\n", 1);
        }
        _exit(0);
    }
    if (!strcmp(mode, "--fifo")) {
        struct stat status;
        if (fstat(STDIN_FILENO, &status) < 0 || !S_ISFIFO(status.st_mode)) _exit(91);
    } else if (strcmp(mode, "--echo")) _exit(92);
    uint8_t block[16384];
    for (;;) {
        ssize_t count = read(STDIN_FILENO, block, sizeof block);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) _exit(93);
        if (!count) _exit(0);
        emit(STDOUT_FILENO, block, (size_t)count);
    }
}
static int execute(const char *const *arguments, size_t count, gp_input cwd,
                   gp_input sets, gp_input removes, gp_input input,
                   int64_t config[GP_CONFIG_WORDS], int64_t meta[6]) {
    uint8_t packed[8192]; size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t size = strlen(arguments[i]) + 1;
        assert(size <= sizeof packed - used);
        memcpy(packed + used, arguments[i], size); used += size;
    }
    return (int)gp_execute(bytes(packed, used), cwd, sets, removes, input,
        (gp_metadata){config, GP_CONFIG_WORDS}, (gp_output){output_bytes, config[OUT_LIMIT]},
        (gp_output){error_bytes, config[ERR_LIMIT]}, (gp_metadata){meta, 6});
}
static int run(const char *mode, gp_input sets, gp_input removes, gp_input input,
               int64_t config[GP_CONFIG_WORDS], int64_t meta[6]) {
    const char *arguments[] = {executable, mode};
    return execute(arguments, 2, bytes(NULL, 0), sets, removes, input, config, meta);
}
static void completed(int result, const int64_t meta[6]) {
    assert(result == GP_OK && meta[0] == 0 && meta[1] == 0 && meta[2] == 0 && meta[5] == 0);
}
static void input_and_faults(void) {
    int64_t config[GP_CONFIG_WORDS], meta[6]; defaults(config); config[IN_MODE] = GP_IN_BYTES;
    for (size_t i = 0; i < sizeof input_bytes; ++i) input_bytes[i] = (uint8_t)i;
    int before = descriptors();
    for (fault = 0; fault <= 4; ++fault) {
        fault_hit = shutdown_calls = 0;
        int result = run("--echo", bytes(NULL, 0), bytes(NULL, 0),
                         bytes(input_bytes, sizeof input_bytes), config, meta);
        if (!fault) {
            completed(result, meta);
            assert(meta[3] == GP_INPUT && meta[4] == 0);
            assert(!memcmp(output_bytes, input_bytes, sizeof input_bytes));
        } else {
            assert(fault_hit == 1 && result == GP_SYSTEM && meta[5] == 0);
            assert(meta[0] == (fault == 4 ? EPERM : EIO));
        }
        assert(descriptors() == before);
    }
    fault = 0;
    struct sigaction disposition_before, disposition_after;
    sigset_t mask_before, mask_after, pending_before, pending_after;
    assert(sigaction(SIGPIPE, NULL, &disposition_before) == 0);
    assert(sigprocmask(SIG_SETMASK, NULL, &mask_before) == 0);
    assert(sigpending(&pending_before) == 0);
    for (int i = 0; i < 20; ++i) {
        completed(run("--exit", bytes(NULL, 0), bytes(NULL, 0),
                      bytes(input_bytes, sizeof input_bytes), config, meta), meta);
        assert(meta[3] == 0 && meta[4] == 0 && descriptors() == before);
    }
    assert(sigaction(SIGPIPE, NULL, &disposition_after) == 0);
    assert(sigprocmask(SIG_SETMASK, NULL, &mask_after) == 0);
    assert(sigpending(&pending_after) == 0);
    assert(disposition_before.sa_handler == disposition_after.sa_handler);
    assert(disposition_before.sa_flags == disposition_after.sa_flags);
    for (int signal = 1; signal < NSIG; ++signal) {
        assert(sigismember(&disposition_before.sa_mask, signal) == sigismember(&disposition_after.sa_mask, signal));
        assert(sigismember(&mask_before, signal) == sigismember(&mask_after, signal));
    }
    assert(sigismember(&pending_before, SIGPIPE) == sigismember(&pending_after, SIGPIPE));
    /* A pending caller SIGPIPE must survive early child closure as well. */
    sigset_t blocked; assert(sigemptyset(&blocked) == 0); assert(sigaddset(&blocked, SIGPIPE) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, NULL) == 0);
    struct sigaction pending_disposition = {0}; pending_disposition.sa_handler = SIG_DFL;
    assert(sigemptyset(&pending_disposition.sa_mask) == 0);
    assert(sigaction(SIGPIPE, &pending_disposition, NULL) == 0);
    bool already_pending = sigismember(&pending_before, SIGPIPE) == 1;
    if (!already_pending) assert(raise(SIGPIPE) == 0);
    completed(run("--exit", bytes(NULL, 0), bytes(NULL, 0),
                  bytes(input_bytes, sizeof input_bytes), config, meta), meta);
    assert(sigpending(&pending_after) == 0 && sigismember(&pending_after, SIGPIPE) == 1);
    if (!already_pending) {
        const struct timespec immediate = {0, 0};
        assert(sigtimedwait(&blocked, NULL, &immediate) == SIGPIPE);
    }
    assert(sigaction(SIGPIPE, &disposition_before, NULL) == 0);
    assert(sigprocmask(SIG_SETMASK, &mask_before, NULL) == 0);
    assert(descriptors() == before);
}
static void environment_cases(void) {
    int64_t config[GP_CONFIG_WORDS], meta[6]; defaults(config);
    const char *keys[] = {keep_key, remove_key, empty_key}; char *old[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        const char *value = getenv(keys[i]);
        if (value) { old[i] = strdup(value); assert(old[i]); }
        assert(setenv(keys[i], "old", 1) == 0);
    }
    const char sets[] = "GOOSE_PROCESS_TEST_KEEP=new\0GOOSE_PROCESS_TEST_EMPTY=\0";
    const char removes[] = "GOOSE_PROCESS_TEST_REMOVE\0";
    completed(run("--selected-environment", bytes(sets, sizeof sets - 1),
                  bytes(removes, sizeof removes - 1), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 13 && !memcmp(output_bytes, "new\n(unset)\n\n", 13));
    for (size_t i = 0; i < 3; ++i) assert(!strcmp(getenv(keys[i]), "old"));
    config[CLEAR_ENV] = 1;
    completed(run("--environment", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 0 && meta[4] == 0);
    completed(run("--environment", bytes(sets, sizeof sets - 1),
                  bytes(removes, sizeof removes - 1), bytes(NULL, 0), config, meta), meta);
    const char expected[] = "GOOSE_PROCESS_TEST_KEEP=new\nGOOSE_PROCESS_TEST_EMPTY=\n";
    assert(meta[3] == (int64_t)sizeof expected - 1 && !memcmp(output_bytes, expected, sizeof expected - 1));
    const char duplicate_sets[] = "KEY=one\0KEY=two\0", one_set[] = "KEY=one\0";
    const char duplicate_removes[] = "KEY\0KEY\0", one_remove[] = "KEY\0";
    assert(run("--exit", bytes(duplicate_sets, sizeof duplicate_sets - 1), bytes(NULL, 0),
               bytes(NULL, 0), config, meta) == GP_INVALID);
    assert(run("--exit", bytes(NULL, 0), bytes(duplicate_removes, sizeof duplicate_removes - 1),
               bytes(NULL, 0), config, meta) == GP_INVALID);
    assert(run("--exit", bytes(one_set, sizeof one_set - 1), bytes(one_remove, sizeof one_remove - 1),
               bytes(NULL, 0), config, meta) == GP_INVALID);
    for (size_t i = 0; i < 3; ++i) {
        assert((old[i] ? setenv(keys[i], old[i], 1) : unsetenv(keys[i])) == 0); free(old[i]);
    }
}
static void join(char *target, size_t capacity, const char *directory, const char *name) {
    int size = snprintf(target, capacity, "%s/%s", directory, name);
    assert(size >= 0 && (size_t)size < capacity);
}
static void path_cases(void) {
    char directory[] = "/tmp/goose-process-extended-XXXXXX"; assert(mkdtemp(directory));
    char denied[PATH_MAX], found[PATH_MAX], denied_program[PATH_MAX], found_program[PATH_MAX];
    char script[PATH_MAX], local_program[PATH_MAX], name[64], sets[2 * PATH_MAX + 16];
    assert(snprintf(name, sizeof name, "goose-process-probe-%ld", (long)getpid()) > 0);
    join(denied, sizeof denied, directory, "denied"); assert(mkdir(denied, 0700) == 0);
    join(found, sizeof found, directory, "found"); assert(mkdir(found, 0700) == 0);
    join(denied_program, sizeof denied_program, denied, name);
    int fd = open(denied_program, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    emit(fd, "not executable\n", 15); assert(close(fd) == 0);
    join(found_program, sizeof found_program, found, name); assert(symlink(executable, found_program) == 0);
    join(local_program, sizeof local_program, directory, name); assert(symlink(executable, local_program) == 0);
    join(script, sizeof script, directory, "script");
    fd = open(script, O_WRONLY | O_CREAT | O_EXCL, 0700); assert(fd >= 0);
    emit(fd, "exit 0\n", 7); assert(close(fd) == 0);
    int64_t config[GP_CONFIG_WORDS], meta[6]; defaults(config);
    config[CLEAR_ENV] = config[SEARCH_PATH] = 1;
    int size = snprintf(sets, sizeof sets, "PATH=%s:%s", denied, found);
    assert(size > 0 && (size_t)size < sizeof sets);
    const char *arguments[] = {name, "--streams"};
    completed(execute(arguments, 2, bytes(NULL, 0), bytes(sets, (size_t)size + 1),
                      bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 3 && !memcmp(output_bytes, "OUT", 3));
    assert(meta[4] == 3 && !memcmp(error_bytes, "ERR", 3));
    const char relative_path[] = "PATH=found\0", empty_path[] = "PATH=\0";
    completed(execute(arguments, 2, text(directory), bytes(relative_path, sizeof relative_path - 1),
                      bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    completed(execute(arguments, 2, text(directory), bytes(empty_path, sizeof empty_path - 1),
                      bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    const char *no_shell[] = {"script"};
    int result = execute(no_shell, 1, text(directory), bytes(empty_path, sizeof empty_path - 1),
                         bytes(NULL, 0), bytes(NULL, 0), config, meta);
    assert(result == GP_LAUNCH && meta[0] == ENOEXEC && meta[1] == -1 && meta[5] == 0);
    /* If every candidate fails, preserve EACCES rather than the later ENOENT. */
    size = snprintf(sets, sizeof sets, "PATH=%s:/no/such/goose-process-directory", denied);
    assert(size > 0 && (size_t)size < sizeof sets);
    result = execute(arguments, 2, bytes(NULL, 0), bytes(sets, (size_t)size + 1),
                     bytes(NULL, 0), bytes(NULL, 0), config, meta);
    assert(result == GP_LAUNCH && meta[0] == EACCES && meta[5] == 0);
    assert(unlink(script) == 0 && unlink(local_program) == 0);
    assert(unlink(found_program) == 0 && unlink(denied_program) == 0);
    assert(rmdir(found) == 0 && rmdir(denied) == 0 && rmdir(directory) == 0);
}
static void streams_and_fds(void) {
    int64_t config[GP_CONFIG_WORDS], meta[6]; defaults(config);
    config[OUT_MODE] = GP_OUT_NULL;
    completed(run("--streams", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 0 && meta[4] == 3 && !memcmp(error_bytes, "ERR", 3));
    config[OUT_MODE] = GP_OUT_CAPTURE; config[ERR_MODE] = GP_OUT_NULL;
    completed(run("--streams", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 3 && !memcmp(output_bytes, "OUT", 3) && meta[4] == 0);
    config[ERR_MODE] = GP_OUT_MERGE; config[OUT_LIMIT] = 6;
    completed(run("--streams", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 6 && !memcmp(output_bytes, "OUTERR", 6) && meta[4] == 0);
    config[OUT_LIMIT] = 5;
    assert(run("--streams", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta) == GP_LIMIT);
    assert(meta[3] == 5 && !memcmp(output_bytes, "OUTER", 5) && meta[5] == 0);
    int pipe_fds[2]; assert(pipe2(pipe_fds, O_CLOEXEC) == 0);
    emit(pipe_fds[1], "q", 1); assert(close(pipe_fds[1]) == 0);
    defaults(config); config[IN_MODE] = GP_IN_FD; config[IN_FD] = pipe_fds[0];
    int pipe_flags = fcntl(pipe_fds[0], F_GETFL); assert(pipe_flags >= 0);
    completed(run("--fifo", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta), meta);
    assert(meta[3] == 1 && output_bytes[0] == 'q');
    assert(fcntl(pipe_fds[0], F_GETFL) == pipe_flags && close(pipe_fds[0]) == 0);
    /* Swap caller stdout/stderr through stable snapshots, preserving flags. */
    char first_name[] = "/tmp/goose-process-fd-a-XXXXXX", second_name[] = "/tmp/goose-process-fd-b-XXXXXX";
    int first = mkstemp(first_name), second = mkstemp(second_name); assert(first >= 0 && second >= 0);
    assert(unlink(first_name) == 0 && unlink(second_name) == 0);
    int saved_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    int saved_err = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3); assert(saved_out >= 0 && saved_err >= 0);
    assert(fflush(stdout) == 0 && fflush(stderr) == 0);
    assert(dup2(first, STDOUT_FILENO) == STDOUT_FILENO && dup2(second, STDERR_FILENO) == STDERR_FILENO);
    int out_flags = fcntl(STDOUT_FILENO, F_GETFL), err_flags = fcntl(STDERR_FILENO, F_GETFL);
    defaults(config); config[OUT_MODE] = config[ERR_MODE] = GP_OUT_FD;
    config[OUT_FD] = STDERR_FILENO; config[ERR_FD] = STDOUT_FILENO;
    int result = run("--streams", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta);
    bool flags_unchanged = fcntl(STDOUT_FILENO, F_GETFL) == out_flags && fcntl(STDERR_FILENO, F_GETFL) == err_flags;
    assert(dup2(saved_out, STDOUT_FILENO) == STDOUT_FILENO && dup2(saved_err, STDERR_FILENO) == STDERR_FILENO);
    assert(close(saved_out) == 0 && close(saved_err) == 0);
    completed(result, meta); assert(flags_unchanged && meta[3] == 0 && meta[4] == 0);
    char content[3]; assert(lseek(first, 0, SEEK_SET) == 0 && read(first, content, 3) == 3 && !memcmp(content, "ERR", 3));
    assert(lseek(second, 0, SEEK_SET) == 0 && read(second, content, 3) == 3 && !memcmp(content, "OUT", 3));
    assert(close(first) == 0 && close(second) == 0);
    /* The closed low source must not be resurrected by snapshotting fd 200. */
    int source = open("/dev/null", O_RDONLY | O_CLOEXEC); assert(source >= 3);
    int high_source = fcntl(source, F_DUPFD_CLOEXEC, 200); assert(high_source >= 200);
    int closed_source = source; assert(close(source) == 0);
    defaults(config); config[IN_MODE] = GP_IN_FD; config[IN_FD] = high_source;
    config[OUT_MODE] = GP_OUT_FD; config[OUT_FD] = closed_source;
    int before = descriptors();
    result = run("--exit", bytes(NULL, 0), bytes(NULL, 0), bytes(NULL, 0), config, meta);
    assert(result == GP_SYSTEM && meta[0] == EBADF && meta[5] == 0);
    assert(fcntl(closed_source, F_GETFD) == -1 && errno == EBADF && descriptors() == before);
    assert(close(high_source) == 0);
}
int main(int argc, char **argv) {
    if (argc == 2) child_mode(argv[1]);
    assert(argc == 1 && realpath(argv[0], executable));
    int before = descriptors();
    input_and_faults(); environment_cases(); path_cases(); streams_and_fds();
    assert(descriptors() == before);
    puts("extended native process tests passed");
    return 0;
}
