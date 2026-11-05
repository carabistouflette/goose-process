/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void emit(int fd, const void *bytes, size_t length) {
    const uint8_t *data = bytes;
    while (length) {
        ssize_t count = write(fd, data, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) _exit(90);
        data += count; length -= (size_t)count;
    }
}
static long number(const char *text) {
    char *end; errno = 0;
    long result = strtol(text, &end, 10);
    if (errno || !*text || *end || result < 0 || result > 1048577) _exit(91);
    return result;
}
static void fill(int fd, uint8_t byte, long length) {
    uint8_t block[4096]; memset(block, byte, sizeof block);
    while (length) {
        size_t take = length < (long)sizeof block ? (size_t)length : sizeof block;
        emit(fd, block, take); length -= (long)take;
    }
}
static void copy_input(void) {
    uint8_t block[4096];
    for (;;) {
        ssize_t count = read(0, block, sizeof block);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) _exit(92);
        if (!count) return;
        emit(1, block, (size_t)count);
    }
}
static void idle(void) {
    struct timespec delay = {60, 0};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}
int main(int argc, char **argv) {
    if (argc < 2) return 91;
    if (!strcmp(argv[1], "source") && argc == 4) {
        fill(2, 'y', number(argv[3])); fill(1, 'x', number(argv[2])); copy_input();
    } else if (!strcmp(argv[1], "copy") && argc == 3) {
        fill(2, 'z', number(argv[2])); copy_input();
    } else if (!strcmp(argv[1], "args")) {
        for (int i = 2; i < argc; ++i) emit(1, argv[i], strlen(argv[i]) + 1);
    } else if (!strcmp(argv[1], "exit") && argc == 3) {
        long code = number(argv[2]); return code < 126 ? (int)code : 91;
    } else if (!strcmp(argv[1], "idle") && argc == 2) {
        idle();
    } else if (!strcmp(argv[1], "prefix") && argc == 2) {
        emit(1, "ready", 5); emit(2, "first\n", 6); idle();
    } else if (!strcmp(argv[1], "flood") && argc == 2) {
        for (;;) fill(1, 'x', 4096);
    } else if (!strcmp(argv[1], "mark") && argc == 3) {
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return 93;
        emit(fd, "x", 1); if (close(fd) < 0) return 93;
    } else return 91;
    return 0;
}
