/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_PROCESS_H
#define GOOSE_PROCESS_H
#include <stdint.h>
typedef struct { const uint8_t *data; int64_t len; } gp_input;
typedef struct { uint8_t *data; int64_t len; } gp_output;
typedef struct { int64_t *data; int64_t len; } gp_metadata;
/* Internal ABI. No pointers are retained. See process.goose for the public API. */
int64_t gp_run(gp_input packed, gp_input cwd, int64_t timeout_ms,
               int64_t capture, gp_output out, gp_output err, gp_metadata meta);
int64_t gp_execute(gp_input packed, gp_input cwd, gp_input sets, gp_input removes,
    gp_input input, gp_metadata config, gp_output out, gp_output err, gp_metadata meta);
int64_t gp_start(gp_input packed, gp_input cwd, gp_input sets, gp_input removes,
    int64_t input_size, gp_metadata config, gp_metadata state, gp_metadata meta);
int64_t gp_wait(gp_metadata state, gp_input input, gp_output out, gp_output err,
    int64_t slice_ms, int64_t one_step, gp_metadata meta, int64_t *done);
int64_t gp_try_wait(gp_metadata state, gp_metadata meta, int64_t *done);
int64_t gp_signal(gp_metadata state, int64_t signal, gp_metadata meta);
int64_t gp_close(gp_metadata state, gp_metadata meta);
int64_t gp_can_start(gp_metadata state);
int64_t gp_id(gp_metadata state);
#endif
