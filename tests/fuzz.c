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
    if (size < 5 || size > 1048704) return 0;
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
    if (size < 9) return 0;
    size_t lengths[4] = {data[4] % 17, data[5] % 33, data[6] % 33, data[7] % 33};
    size_t offset = 9;
    gp_input fields[4];
    for (size_t i = 0; i < 4; ++i) {
        if (lengths[i] > size - offset) return 0;
        fields[i] = (gp_input){data + offset, (int64_t)lengths[i]};
        offset += lengths[i];
    }
    int64_t config[11] = {data[2] & 1 ? 1000 : -1,
        1 + data[3] % 6, 1 + data[8] % 6, data[8] % 5,
        -1, -1, -1, (data[3] >> 3) & 1, (data[3] >> 4) & 1, cap, cap};
    result = (int)gp_execute((gp_input){data + offset, (int64_t)(size - offset)},
        fields[0], fields[1], fields[2], fields[3], (gp_metadata){config, 11},
        (gp_output){out, cap}, (gp_output){err, cap}, (gp_metadata){meta, 6});
    assert(result == 1 || result == 2 || result == 3);
    assert(meta[5] == 0);
    for (size_t i = 0; i < sizeof out; ++i) assert(out[i] == 0xa5 && err[i] == 0x5a);
    /* Include start-only routing (Input.Stream) without ever executing mutated
       bytes: the posix_spawn wrapper above always refuses execution. */
    int64_t storage[24] = {0};
    result = (int)gp_start((gp_input){data + offset, (int64_t)(size - offset)}, fields[0], fields[1], fields[2],
                           0, (gp_metadata){config, 11}, (gp_metadata){storage, 24}, (gp_metadata){meta, 6});
    assert(result == 1 || result == 2 || result == 3);
    assert(meta[5] == 0);
    /* Both pipeline commands remain under the unconditional spawn refusal. */
    int64_t pipe_state[55] = {0}, pipe_meta[17] = {0}, second[11];
    for (size_t i = 0; i < 11; ++i) second[i] = config[i];
    second[3] = 0;
    result = (int)gp_pipeline_start((gp_input){data + offset, (int64_t)(size - offset)},
        fields[0], fields[1], fields[2], (gp_input){data + offset, (int64_t)(size - offset)},
        fields[0], fields[1], fields[2], 0, (gp_metadata){config, 11}, (gp_metadata){second, 11},
        (gp_metadata){pipe_state, 55}, (gp_metadata){pipe_meta, 17});
    assert(result >= 1 && result <= 4);
    assert(pipe_meta[8] == 0 && pipe_meta[14] == 0 && !pipe_meta[15] && !pipe_meta[16]);
    return 0;
}
