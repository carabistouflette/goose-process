/* SPDX-License-Identifier: Apache-2.0 */
/* Test-only variants of the exact worker, selected by executable basename. */
#define main worker_main
#include "../../examples/worker.c"
#undef main
#include <string.h>
int main(int argc, char **argv) {
    (void)argc;
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "frontload")) {
        uint8_t frame[35] = {0, 0, 0, 31};
        static const uint8_t ack[4] = {'a', 'c', 'k', 0};
        for (size_t i = 0; i < 31; ++i) frame[i + 4] = (uint8_t)((i * 17 + i / 251) % 256);
        for (int i = 0; i < 128; ++i) if (emit(1, frame, sizeof frame)) return 3;
        for (int i = 0; i < 128; ++i) if (emit(2, ack, sizeof ack)) return 3;
        uint8_t input[4096];
        while (read(0, input, sizeof input) > 0) {}
        return 0;
    }
    if (!strcmp(name, "early")) return 0;
    if (!strcmp(name, "corrupt")) {
        const uint8_t bad = 255;
        if (emit(1, &bad, 1)) return 3;
        return worker_main();
    }
    if (!strcmp(name, "excess")) {
        int status = worker_main();
        const uint8_t bad = 255;
        if (status) return status;
        return emit(1, &bad, 1) ? 3 : 0;
    }
    return 2;
}
