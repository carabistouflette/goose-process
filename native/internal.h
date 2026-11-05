/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_PROCESS_INTERNAL_H
#define GOOSE_PROCESS_INTERNAL_H
#include "process.h"
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>

enum { GP_OK, GP_INVALID, GP_LAUNCH, GP_SYSTEM, GP_TIMEOUT, GP_LIMIT, GP_STATE };
enum { GP_ARGS = 4096, GP_ARG_BYTES = 1048576, GP_ARG = 131071,
       GP_PATH = 4095, GP_OUTPUT = 1048576, GP_INPUT = 1048576,
       GP_ENV_BYTES = 1048576, GP_ENV_ITEM = 131071, GP_ENV_COUNT = 4096,
       GP_ENV_CHANGES = 256, GP_ENV_KEY = 256, GP_SEARCH_BYTES = 16384,
       GP_SEARCH_PARTS = 256 };
enum { GP_IN_NULL, GP_IN_INHERIT, GP_IN_BYTES, GP_IN_FD, GP_IN_STREAM };
enum { GP_OUT_DEFAULT, GP_OUT_CAPTURE, GP_OUT_INHERIT, GP_OUT_NULL,
       GP_OUT_MERGE, GP_OUT_FD, GP_OUT_STREAM };
enum { GP_CONFIG_WORDS = 11, GP_CHILD_WORDS = 24, GP_PIPE_WORDS = 55, GP_PIPE_META = 17 };

typedef struct {
    int64_t timeout_ms, out_mode, err_mode, in_mode, out_fd, err_fd, in_fd;
    int64_t clear_env, search_path, out_limit, err_limit;
} gp_config;

typedef struct {
    char **argv, **envp;
    void *argument_memory, *environment_memory;
    const char *search;
    size_t search_length;
} gp_command;

int gp_command_prepare(gp_input packed, gp_input sets, gp_input removes,
                       const gp_config *config, gp_command *command, int *error);
void gp_command_destroy(gp_command *command);
int gp_command_spawn(const gp_command *command, const gp_config *config,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    int64_t deadline, pid_t *pid, int *error);
int64_t gp_clock_ns(void);
#endif
