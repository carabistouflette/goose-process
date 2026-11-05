/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../native/process.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

static bool fail_read;
ssize_t __real_read(int fd, void *data, size_t size);
ssize_t __wrap_read(int fd, void *data, size_t size) {
    if (fail_read) { errno = EIO; return -1; }
    return __real_read(fd, data, size);
}
static gp_input empty(void) { return (gp_input){NULL, 0}; }
static gp_metadata words(int64_t *data, int64_t size) { return (gp_metadata){data, size}; }
static int descriptors(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    int count = 0; while (readdir(directory)) ++count;
    assert(closedir(directory) == 0); return count;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "held")) {
        pid_t descendant = fork(); assert(descendant >= 0);
        if (!descendant) { sleep(2); _exit(0); }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "prefix")) {
        assert(write(1, "abc", 3) == 3); usleep(20000);
        assert(write(1, "z", 1) == 1); sleep(10); return 0;
    }
    char self[4096]; ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(length > 0); self[length] = 0;
    uint8_t packed[8192]; memcpy(packed, self, (size_t)length + 1);
    memcpy(packed + length + 1, "prefix", 7);
    gp_input command = {packed, length + 8};
    int before = descriptors();
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    for (int fault = 0; fault < 2; ++fault) {
        int64_t state[24] = {0}, meta[6], done = 0;
        int64_t config[11] = {1000, 6, 3, 0, -1, -1, -1, 0, 0, 0, 0};
        assert(gp_start(command, empty(), empty(), empty(), 0, words(config, 11), words(state, 24), words(meta, 6)) == 0);
        pid_t pid = (pid_t)gp_id(words(state, 24)); assert(pid > 0);
        uint8_t buffer[3] = {0}; gp_output out = {buffer, sizeof buffer}, none = {NULL, 0};
        assert(gp_stream_step(words(state, 24), empty(), (gp_output){(uint8_t *)state, 3}, none,
                              0, words(meta, 6), &done) == 1);
        assert(gp_wait(words(state, 24), empty(), out, none, 0, 1, words(meta, 6), &done) == 6);
        assert(gp_id(words(state, 24)) == pid);
        assert(gp_stream_step(words(state, 24), empty(), out, none, 100, words(meta, 6), &done) == 0);
        assert(meta[3] == 3 && !done && !memcmp(buffer, "abc", 3));
        if (fault) fail_read = true;
        int result;
        do {
            result = (int)gp_stream_step(words(state, 24), empty(), out, none, 100, words(meta, 6), &done);
        } while (!done);
        fail_read = false;
        assert(result == (fault ? 3 : 4) && meta[3] == 0 && meta[5] == 0);
        assert(meta[2] == SIGKILL && gp_id(words(state, 24)) == -1);
        errno = 0; assert(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
        assert(gp_stream_step(words(state, 24), empty(), out, none, 0, words(meta, 6), &done) == result);
        assert(done && meta[3] == 0);
        assert(gp_close(words(state, 24), words(meta, 6)) == result && meta[5] == 0);
    }
    memcpy(packed + length + 1, "held", 5); command.len = length + 6;
    int64_t state[24] = {0}, meta[6], done = 0;
    int64_t config[11] = {1000, 6, 3, 0, -1, -1, -1, 0, 0, 0, 0, 0};
    assert(gp_start(command, empty(), empty(), empty(), 0, words(config, 11), words(state, 24), words(meta, 6)) == 0);
    uint8_t byte; int result;
    do {
        result = (int)gp_stream_step(words(state, 24), empty(), (gp_output){&byte, 1},
                                    (gp_output){NULL, 0}, 20, words(meta, 6), &done);
    } while (!done);
    assert(result == 4 && meta[1] == 0 && meta[5] == 0);
    /* Only after the library has reaped its direct child, reap our adopted
       fixture. No numeric descendant signaling or process-tree claim. */
    assert(waitpid(-1, NULL, 0) > 0);
    errno = 0; assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(descriptors() == before);
    puts("streaming native fault and ownership tests passed");
    return 0;
}
