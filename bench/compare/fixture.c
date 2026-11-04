/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void emit(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(90);
        p += n; size -= (size_t)n;
    }
}
int main(int argc, char **argv) {
    if (argc < 2) return 91;
    if (!strcmp(argv[1], "empty")) return argc == 2 ? 0 : 92;
    if (!strcmp(argv[1], "args")) {
        if (argc != 130) return 93;
        for (int i = 2; i < argc; ++i)
            if (strcmp(argv[i], "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")) return 94;
        return 0;
    }
    if (!strcmp(argv[1], "environment")) {
        const char *value = getenv("PROBE");
        extern char **environ;
        return value && !strcmp(value, "goose") && environ[0] && !environ[1] ? 0 : 95;
    }
    if (!strcmp(argv[1], "input")) {
        unsigned char data[4096]; size_t total = 0;
        for (;;) {
            ssize_t n = read(0, data, sizeof data);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return 96;
            if (!n) break;
            for (ssize_t i = 0; i < n; ++i) if (data[i] != 'x') return 97;
            total += (size_t)n; emit(1, data, (size_t)n);
        }
        return total == 131072 ? 0 : 98;
    }
    if (!strcmp(argv[1], "streams") || !strcmp(argv[1], "redirect")) {
        char out[4096], err[4096]; memset(out, 'x', sizeof out); memset(err, 'y', sizeof err);
        for (int i = 0; i < 16; ++i) { emit(1, out, sizeof out); emit(2, err, sizeof err); }
        return 0;
    }
    return 99;
}
