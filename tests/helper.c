/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ptrace.h>
#include <unistd.h>
static void emit(int fd, const void *data, size_t length) {
    const char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(90);
        p += n; length -= (size_t)n;
    }
}
static void pause_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void mark_state(const char *path, char state) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) _exit(100);
    emit(fd, &state, 1);
    if (close(fd) < 0) _exit(100);
}
int main(int argc, char **argv) {
    if (argc < 2) return 91;
    if (!strcmp(argv[1], "args")) {
        for (int i = 2; i < argc; ++i) emit(1, argv[i], strlen(argv[i]) + 1);
    } else if (!strcmp(argv[1], "bytes")) {
        unsigned char b[256]; for (int i = 0; i < 256; ++i) b[i] = (unsigned char)i;
        emit(1, b, sizeof b);
        for (int i = 0; i < 256; ++i) b[i] = (unsigned char)(255 - i);
        emit(2, b, sizeof b);
    } else if (!strcmp(argv[1], "exit")) {
        return argc > 2 ? atoi(argv[2]) : 0;
    } else if (!strcmp(argv[1], "signal")) {
        raise(SIGTERM); return 92;
    } else if (!strcmp(argv[1], "stdin")) {
        char c; return read(0, &c, 1) == 0 ? 0 : 93;
    } else if (!strcmp(argv[1], "fd")) {
        return fcntl(199, F_GETFD) == -1 && errno == EBADF ? 0 : 94;
    } else if (!strcmp(argv[1], "trace")) {
        /* Some CI sandboxes deny ptrace; deterministic injected tests still run. */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) return 77;
        raise(SIGSTOP);
        return 98;
    } else if (!strcmp(argv[1], "sleep")) {
        pause_ms(argc > 2 ? atol(argv[2]) : 1000);
    } else if (!strcmp(argv[1], "close-sleep")) {
        close(STDOUT_FILENO); close(STDERR_FILENO);
        if (argc > 2) mark_state(argv[2], 'C');
        pause_ms(5000);
    } else if (!strcmp(argv[1], "session-sleep")) {
        if (setsid() < 0) return 99;
        if (argc > 2) mark_state(argv[2], 'S');
        pause_ms(5000);
    } else if (!strcmp(argv[1], "descendant")) {
        pid_t pid = fork(); if (pid < 0) return 95;
        if (pid == 0) { pause_ms(2000); _exit(0); }
    } else if (!strcmp(argv[1], "cwd")) {
        char path[4096]; if (!getcwd(path, sizeof path)) return 96;
        emit(1, path, strlen(path));
    } else if (!strcmp(argv[1], "duplex-burst")) {
        char out[4096], err[4096]; memset(out, 'x', sizeof out); memset(err, 'y', sizeof err);
        for (int i = 0; i < 64; ++i) { emit(1, out, sizeof out); emit(2, err, sizeof err); }
        for (;;) {
            ssize_t count = read(0, out, sizeof out);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) return 102;
            if (!count) break;
            emit(1, out, (size_t)count);
        }
    } else if (!strcmp(argv[1], "close-input")) {
        if (close(0) < 0) return 102;
        emit(1, "closed", 6); pause_ms(60000);
    } else if (!strcmp(argv[1], "binary-streams")) {
        long n = argc > 2 ? atol(argv[2]) : 0, offset = 0;
        unsigned char out[4096], err[4096];
        while (offset < n) {
            size_t take = n - offset < (long)sizeof out ? (size_t)(n - offset) : sizeof out;
            for (size_t i = 0; i < take; ++i) {
                long position = offset + (long)i;
                out[i] = (unsigned char)(position * 17 + position / 251);
                err[i] = (unsigned char)(255 - out[i]);
            }
            emit(1, out, take); emit(2, err, take); offset += (long)take;
        }
    } else if (!strcmp(argv[1], "streams")) {
        long n = argc > 2 ? atol(argv[2]) : 0;
        char block[4096], errors[4096];
        memset(block, 'x', sizeof block); memset(errors, 'y', sizeof errors);
        while (n > 0) {
            size_t take = n < (long)sizeof block ? (size_t)n : sizeof block;
            emit(1, block, take); emit(2, errors, take); n -= (long)take;
        }
    } else if (!strcmp(argv[1], "stderr")) {
        long n = argc > 2 ? atol(argv[2]) : 0;
        char block[4096]; memset(block, 'e', sizeof block);
        while (n > 0) {
            size_t take = n < (long)sizeof block ? (size_t)n : sizeof block;
            emit(2, block, take); n -= (long)take;
        }
    } else if (!strcmp(argv[1], "flood")) {
        char block[4096]; memset(block, 'x', sizeof block);
        for (;;) { emit(1, block, sizeof block); emit(2, block, sizeof block); }
    } else return 97;
    return 0;
}
