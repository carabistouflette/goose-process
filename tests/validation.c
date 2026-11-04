/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <spawn.h>
#include <time.h>

/* The classification oracle must never execute a mutated command. */
int __wrap_posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[]) {
    (void)pid; (void)path; (void)actions; (void)attributes; (void)argv; (void)environment;
    return EACCES;
}

/* Validation cases include a 1ms budget. Freeze time so host scheduling cannot
   turn a valid-input classification into a preparation timeout. */
int __wrap_clock_gettime(clockid_t clock, struct timespec *value) {
    assert(clock == CLOCK_MONOTONIC);
    *value = (struct timespec){1, 0};
    return 0;
}
