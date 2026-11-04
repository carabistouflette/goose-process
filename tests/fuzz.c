/* SPDX-License-Identifier: Apache-2.0 */
#include "../native/process.h"
#include <assert.h>
#include <errno.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>

/* Exercise parsing/setup/cleanup without executing mutated input as a command. */
int __wrap_posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const env[]) {
    (void)pid; (void)path; (void)actions; (void)attributes; (void)argv; (void)env;
    return EACCES;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 5 || size > 65557) return 0;
    size_t cwd_length = data[4] % 17;
    if (cwd_length > size - 5) return 0;
    int64_t cap = data[0] < 128 ? data[0] : -1;
    uint8_t out[143], err[143];
    for (size_t i = 0; i < sizeof out; ++i) { out[i] = 0xa5; err[i] = 0x5a; }
    int64_t meta[6] = {0, -1, 0, 0, 0, 0};
    int result = (int)gp_run(
        (gp_input){data + 5 + cwd_length, (int64_t)(size - 5 - cwd_length)},
        (gp_input){data + 5, (int64_t)cwd_length}, data[2] & 1 ? 1000 : -1,
        data[3] & 3, (gp_output){out, cap}, (gp_output){err, cap},
        (gp_metadata){meta, data[1] % 8});
    assert(result == 1 || result == 2 || result == 3);
    assert(meta[5] == 0);
    for (size_t i = 0; i < sizeof out; ++i) assert(out[i] == 0xa5 && err[i] == 0x5a);
    return 0;
}
