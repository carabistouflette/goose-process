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
