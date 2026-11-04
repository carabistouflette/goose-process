/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static const char *helper;
static int64_t meta[6];
static uint8_t out[1048576], err[1048576];
static double now(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int run(const char *mode, const char *arg, int limit, int timeout, int capture) {
    uint8_t packed[65536];
    size_t length = strlen(helper) + 1;
    memcpy(packed, helper, length);
    size_t n = strlen(mode) + 1; memcpy(packed + length, mode, n); length += n;
    if (arg) { n = strlen(arg) + 1; memcpy(packed + length, arg, n); length += n; }
    return (int)gp_run((gp_input){packed, (int64_t)length}, (gp_input){NULL, 0},
        timeout, capture, (gp_output){out, limit}, (gp_output){err, limit}, (gp_metadata){meta, 6});
}
static int descriptors(void) {
    DIR *dir = opendir("/proc/self/fd"); assert(dir);
    int n = 0; while (readdir(dir)) ++n;
    closedir(dir); return n;
}
static void guarded_boundaries(void) {
    long page = sysconf(_SC_PAGESIZE); assert(page >= 4096);
    uint8_t *regions[5], *ends[5];
    for (int i = 0; i < 5; ++i) {
        regions[i] = mmap(NULL, (size_t)page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(regions[i] != MAP_FAILED);
        assert(mprotect(regions[i] + page, (size_t)page, PROT_READ | PROT_WRITE) == 0);
        ends[i] = regions[i] + page * 2;
    }
    size_t length = strlen(helper) + 1 + sizeof "bytes";
    assert(length <= (size_t)page);
    uint8_t *packed = ends[0] - length;
    memcpy(packed, helper, strlen(helper) + 1);
    memcpy(packed + strlen(helper) + 1, "bytes", sizeof "bytes");
    uint8_t *cwd = ends[1] - 4; memcpy(cwd, "/tmp", 4);
    assert(mprotect(regions[0] + page, (size_t)page, PROT_READ) == 0);
    assert(mprotect(regions[1] + page, (size_t)page, PROT_READ) == 0);
    int64_t *metadata = (int64_t *)(ends[4] - 6 * sizeof(int64_t));
    int result = (int)gp_run((gp_input){packed, (int64_t)length}, (gp_input){cwd, 4}, 2000, 1,
        (gp_output){ends[2] - 256, 256}, (gp_output){ends[3] - 256, 256}, (gp_metadata){metadata, 6});
    assert(result == 0 && metadata[1] == 0 && metadata[3] == 256 && metadata[4] == 256);
    for (int i = 0; i < 256; ++i) {
        assert((ends[2] - 256)[i] == i && (ends[3] - 256)[i] == 255 - i);
    }
    /* Empty spans may point immediately at an inaccessible page. */
    result = (int)gp_run((gp_input){packed, (int64_t)length}, (gp_input){ends[1], 0}, 2000, 1,
        (gp_output){ends[2], 0}, (gp_output){ends[3], 0}, (gp_metadata){metadata, 6});
    assert(result == 5 && metadata[3] == 0 && metadata[4] == 0 && metadata[5] == 0);
    result = (int)gp_run((gp_input){ends[0], 0}, (gp_input){ends[1], 0}, 2000, 1,
        (gp_output){ends[2], 0}, (gp_output){ends[3], 0}, (gp_metadata){metadata, 6});
    assert(result == 1);
    for (int i = 0; i < 5; ++i) assert(munmap(regions[i], (size_t)page * 3) == 0);
}
static void malformed(void) {
    uint8_t good[] = "/bin/true\0";
    gp_input input = {good, sizeof good - 1}, cwd = {NULL, 0};
    gp_output a = {out, 10}, b = {err, 10};
    gp_metadata m = {meta, 6};
    assert(gp_run(input, cwd, 1000, 1, a, b, (gp_metadata){meta, 4}) == 1);
    assert(gp_run((gp_input){NULL, 10}, cwd, 1000, 1, a, b, m) == 1);
    assert(gp_run((gp_input){good, -1}, cwd, 1000, 1, a, b, m) == 1);
    assert(gp_run(input, (gp_input){good, -1}, 1000, 1, a, b, m) == 1);
    assert(gp_run(input, (gp_input){good, 3}, 1000, 1, a, b, m) == 2);
    assert(gp_run(input, (gp_input){good, sizeof good}, 1000, 1, a, b, m) == 1);
    assert(gp_run(input, cwd, 0, 1, a, b, m) == 1);
    assert(gp_run(input, cwd, 86400001, 1, a, b, m) == 1);
    assert(gp_run(input, cwd, 1000, 2, a, b, m) == 1);
    assert(gp_run(input, cwd, 1000, 1, a, a, m) == 1);
    assert(gp_run(input, cwd, 1000, 1, a, (gp_output){out + 5, 10}, m) == 1);
    meta[3] = 123;
    assert(gp_run(input, cwd, 1000, 1, (gp_output){(uint8_t *)meta + 24, 10}, b, m) == 1);
    assert(meta[3] == 123); /* Reject before metadata initialization or capture. */
    assert(gp_run((gp_input){(uint8_t *)meta, 16}, cwd, 1000, 1, a, b, m) == 1);
    assert(meta[3] == 123);
    assert(gp_run(input, cwd, 1000, 1, (gp_output){NULL, 1}, b, m) == 1);
    assert(gp_run(input, cwd, 1000, 1, (gp_output){out, 1048577}, b, m) == 1);
    good[0] = 'x'; assert(gp_run(input, cwd, 1000, 1, a, b, m) == 1);
    good[0] = '/'; good[sizeof good - 2] = 'x';
    assert(gp_run(input, cwd, 1000, 1, a, b, m) == 1);
    static uint8_t packed[1048577]; memset(packed, 'a', sizeof packed); packed[0] = '/';
    packed[sizeof packed - 1] = 0;
    assert(gp_run((gp_input){packed, sizeof packed}, cwd, 1000, 1, a, b, m) == 1);
    packed[4096] = 0;
    assert(gp_run((gp_input){packed, 4097}, cwd, 1000, 1, a, b, m) == 1);
    memset(packed, 0, 4098); packed[0] = '/';
    assert(gp_run((gp_input){packed, 4098}, cwd, 1000, 1, a, b, m) == 1);
    memset(packed, 'a', 131075); packed[0] = '/'; packed[1] = 0; packed[131074] = 0;
    assert(gp_run((gp_input){packed, 131075}, cwd, 1000, 1, a, b, m) == 1);
}
static void *parallel(void *unused) {
    (void)unused;
    uint8_t packed[] = "/bin/true\0";
    for (int i = 0; i < 30; ++i) {
        int64_t m[6]; uint8_t a[1], b[1];
        assert(gp_run((gp_input){packed, sizeof packed - 1}, (gp_input){NULL, 0}, 2000, 1,
            (gp_output){a, 1}, (gp_output){b, 1}, (gp_metadata){m, 6}) == 0);
        assert(m[1] == 0);
    }
    return NULL;
}
static void closed_stdio(void) {
    for (int mask = 0; mask < 8; ++mask) {
        pid_t pid = fork(); assert(pid >= 0);
        if (pid == 0) {
            for (int fd = 0; fd < 3; ++fd) if (mask & (1 << fd)) close(fd);
            int r = run("bytes", NULL, 256, 2000, 1);
            _exit(r == 0 && meta[1] == 0 && meta[3] == 256 && meta[4] == 256 ? 0 : 1);
        }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
static void exhausted_descriptors(void) {
    struct rlimit original;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    int count = descriptors();
    for (rlim_t limit = 3; limit <= 15 && limit <= original.rlim_cur; ++limit) {
        struct rlimit bounded = original; bounded.rlim_cur = limit;
        assert(setrlimit(RLIMIT_NOFILE, &bounded) == 0);
        int result = run("exit", NULL, 1, 2000, 1);
        assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
        assert((result == 0 && meta[1] == 0) || (result == 3 && meta[0] == EMFILE));
        assert(meta[5] == 0 && descriptors() == count);
    }
}
static volatile sig_atomic_t interruptions;
static void alarm_handler(int signal) { (void)signal; ++interruptions; }
static void interrupted_wait(void) {
    struct sigaction action = {0}, old;
    action.sa_handler = alarm_handler; sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM, &action, &old) == 0);
    struct itimerval timer = {{0, 100}, {0, 100}}, previous;
    assert(setitimer(ITIMER_REAL, &timer, &previous) == 0);
    double start = now();
    int result = run("sleep", "2000", 1, 20, 1);
    double elapsed = now() - start;
    sigset_t alarms, old_mask;
    sigemptyset(&alarms); sigaddset(&alarms, SIGALRM);
    assert(pthread_sigmask(SIG_BLOCK, &alarms, &old_mask) == 0);
    timer = (struct itimerval){{0, 0}, {0, 0}};
    assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    struct timespec zero = {0, 0};
    while (sigtimedwait(&alarms, NULL, &zero) >= 0) {}
    assert(errno == EAGAIN);
    assert(sigaction(SIGALRM, &old, NULL) == 0);
    assert(setitimer(ITIMER_REAL, &previous, NULL) == 0);
    assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);
    assert(result == 4 && meta[2] == SIGKILL && meta[5] == 0);
    assert(interruptions > 0 && elapsed < 1.5);
}
int main(int argc, char **argv) {
    assert(argc == 2); helper = argv[1]; assert(helper[0] == '/');
    malformed();
    guarded_boundaries();
    int before = descriptors();
    assert(run("bytes", NULL, 256, 2000, 1) == 0 && meta[1] == 0);
    for (int i = 0; i < 256; ++i) assert(out[i] == i && err[i] == 255 - i);
    assert(run("streams", "1048576", 1048576, 5000, 1) == 0);
    assert(meta[3] == 1048576 && meta[4] == 1048576);
    assert(run("streams", "1048577", 1048576, 5000, 1) == 5);
    assert(run("streams", "0", 0, 2000, 1) == 0);
    assert(run("streams", "1", 0, 2000, 1) == 5);
    assert(run("stderr", "257", 256, 2000, 1) == 5 && meta[3] == 0 && meta[4] == 256);
    for (int i = 0; i < 256; ++i) assert(err[i] == 'e');
    assert(run("stdin", NULL, 0, 2000, 1) == 0 && meta[1] == 0);
    assert(run("signal", NULL, 0, 2000, 1) == 0 && meta[1] == -1 && meta[2] == SIGTERM);
    int traced = run("trace", NULL, 1, 250, 1);
    if (meta[1] == 77) assert(traced == 0); /* ptrace forbidden by host */
    else assert(traced == 4 && meta[2] == SIGKILL && meta[5] == 0);
    assert(run("exit", "127", 0, 2000, 0) == 0 && meta[1] == 127);
    for (int capture = 0; capture <= 1; ++capture) {
        const char *states[] = {"close-sleep", "session-sleep"};
        for (size_t i = 0; i < sizeof states / sizeof states[0]; ++i) {
            char path[] = "/tmp/goose-process-state-XXXXXX";
            int marker = mkostemp(path, O_CLOEXEC); assert(marker >= 0);
            assert(run(states[i], path, 1, 1000, capture) == 4);
            assert(meta[1] == -1 && meta[2] == SIGKILL && meta[5] == 0);
            assert(meta[3] == 0 && meta[4] == 0);
            char reached;
            assert(read(marker, &reached, 1) == 1 && reached == (i == 0 ? 'C' : 'S'));
            assert(close(marker) == 0 && unlink(path) == 0);
        }
    }
    double start = now(); assert(run("sleep", "1000", 0, 20, 1) == 4);
    assert(now() - start < 2.0);
    start = now(); assert(run("descendant", NULL, 1, 250, 1) == 4);
    assert(now() - start < 1.5 && meta[1] == 0);
    /* 1ms cancellation must be checked even during continuous readable output. */
    int r = run("flood", NULL, 1048576, 1, 1); assert(r == 4 || r == 5);
    int fd = open("/dev/null", O_RDONLY); assert(fd >= 0);
    assert(dup2(fd, 199) == 199); close(fd);
    assert(run("fd", NULL, 0, 2000, 1) == 0 && meta[1] == 0); close(199);
    closed_stdio();
    exhausted_descriptors();
    interrupted_wait();
    struct sigaction old, ignored = {0}; ignored.sa_handler = SIG_IGN;
    assert(sigaction(SIGCHLD, &ignored, &old) == 0);
    assert(run("exit", NULL, 0, 2000, 1) == 3 && meta[0] == EINVAL);
    assert(sigaction(SIGCHLD, &old, NULL) == 0);
    struct sigaction old_term;
    assert(sigaction(SIGTERM, &ignored, &old_term) == 0);
    sigset_t blocked, old_mask;
    sigemptyset(&blocked); sigaddset(&blocked, SIGTERM);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, &old_mask) == 0);
    assert(run("signal", NULL, 0, 2000, 1) == 0 && meta[2] == SIGTERM);
    assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);
    assert(sigaction(SIGTERM, &old_term, NULL) == 0);
    const char *original = helper;
    helper = "/no/such/goose-process-executable";
    assert(run("exit", NULL, 0, 2000, 1) == 2 && meta[0] == ENOENT);
    helper = original;
    for (int i = 0; i < 50; ++i) {
        assert(run("exit", NULL, 0, 2000, 1) == 0 && meta[1] == 0);
        assert(run("sleep", NULL, 0, 1, 1) == 4);
        assert(run("streams", "1", 0, 2000, 1) == 5);
    }
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, parallel, NULL) == 0);
    for (int i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(descriptors() == before);
    int status; errno = 0; assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("native process tests passed");
    return 0;
}
