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
#endif
