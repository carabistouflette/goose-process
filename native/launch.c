/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;
static char *const empty_environment[] = {NULL};
typedef struct { char *record; size_t key_length; bool setting; } change;

static bool span(gp_input input, int64_t limit) {
    return input.len >= 0 && input.len <= limit && (input.len == 0 || input.data);
}

/* Validate packed records before allocation or spawning. */
static int changes_scan(gp_input records, bool setting, size_t *count) {
    if (!span(records, GP_ENV_BYTES) || (records.len && records.data[records.len - 1])) return GP_INVALID;
    size_t start = 0;
    for (size_t i = 0; i < (size_t)records.len; ++i) {
        if (records.data[i]) continue;
        size_t length = i - start;
        const uint8_t *equals = memchr(records.data + start, '=', length);
        size_t key = equals ? (size_t)(equals - records.data - start) : length;
        if (!length || !key || key > GP_ENV_KEY || length > GP_ENV_ITEM ||
            (setting ? !equals : equals != NULL) || ++*count > GP_ENV_CHANGES) return GP_INVALID;
        start = i + 1;
    }
    return GP_OK;
}

static int compare(const void *left, const void *right) {
    const change *a = *(const change *const *)left, *b = *(const change *const *)right;
    size_t length = a->key_length < b->key_length ? a->key_length : b->key_length;
    int result = memcmp(a->record, b->record, length);
    if (result) return result;
    return (a->key_length > b->key_length) - (a->key_length < b->key_length);
}

static bool overridden(change *const *index, size_t count, const char *key, size_t length) {
    change query = {(char *)key, length, false};
    change *pointer = &query;
    return bsearch(&pointer, index, count, sizeof *index, compare) != NULL;
}

static int environment(gp_input sets, gp_input removes, bool clear, gp_command *command, int *error) {
    size_t count = 0;
    if (sets.len < 0 || removes.len < 0 || sets.len > GP_ENV_BYTES ||
        removes.len > GP_ENV_BYTES - sets.len || changes_scan(sets, true, &count) ||
        changes_scan(removes, false, &count)) return GP_INVALID;
    if (!clear && !count) {
        command->envp = environ ? environ : (char **)empty_environment;
        return GP_OK;
    }
    size_t inherited = 0;
    if (!clear && environ) {
        while (environ[inherited]) {
            if (inherited == GP_ENV_COUNT) return GP_INVALID;
            ++inherited;
        }
    }
    size_t table_size = (inherited + count + 1) * sizeof(char *);
    size_t change_size = count * sizeof(change);
    void *memory = malloc(table_size + change_size + (size_t)sets.len + (size_t)removes.len);
    if (!memory) { *error = ENOMEM; return GP_SYSTEM; }
    command->environment_memory = memory;
    command->envp = memory;
    change *items = (change *)((char *)memory + table_size);
    char *text = (char *)items + change_size;
    if (sets.len) memcpy(text, sets.data, (size_t)sets.len);
    if (removes.len) memcpy(text + sets.len, removes.data, (size_t)removes.len);
    change *index[GP_ENV_CHANGES];
    size_t used = 0, position = 0;
    while (position < (size_t)(sets.len + removes.len)) {
        char *record = text + position;
        size_t length = strlen(record);
        bool setting = position < (size_t)sets.len;
        char *equals = setting ? memchr(record, '=', length) : NULL;
        items[used] = (change){record, setting ? (size_t)(equals - record) : length, setting};
        index[used] = &items[used];
        ++used; position += length + 1;
    }
    qsort(index, count, sizeof *index, compare);
    for (size_t i = 1; i < count; ++i)
        if (compare(&index[i - 1], &index[i]) == 0) return GP_INVALID;
    size_t entries = 0, bytes = 0;
    for (size_t i = 0; i < inherited; ++i) {
        size_t length = strnlen(environ[i], GP_ENV_ITEM + 1);
        if (length > GP_ENV_ITEM) return GP_INVALID;
        const char *equals = memchr(environ[i], '=', length);
        size_t key_length = equals ? (size_t)(equals - environ[i]) : length;
        if (overridden(index, count, environ[i], key_length)) continue;
        if (bytes > GP_ENV_BYTES - length - 1) return GP_INVALID;
        bytes += length + 1;
        command->envp[entries++] = environ[i];
    }
    for (size_t i = 0; i < count; ++i) {
        if (!items[i].setting) continue;
        size_t length = strlen(items[i].record);
        if (entries == GP_ENV_COUNT || bytes > GP_ENV_BYTES - length - 1) return GP_INVALID;
        bytes += length + 1;
        command->envp[entries++] = items[i].record;
    }
    command->envp[entries] = NULL;
    return GP_OK;
}

int gp_command_prepare(gp_input packed, gp_input sets, gp_input removes,
                       const gp_config *config, gp_command *command, int *error) {
    memset(command, 0, sizeof *command);
    if (!span(packed, GP_ARG_BYTES) || packed.len < 2 || packed.data[0] == 0 ||
        packed.data[packed.len - 1] || (!config->search_path && packed.data[0] != '/')) return GP_INVALID;
    size_t count = 0, start = 0;
    for (size_t i = 0; i < (size_t)packed.len; ++i) {
        if (packed.data[i]) continue;
        size_t length = i - start;
        if (length > GP_ARG || (count == 0 && length > GP_PATH) || ++count > GP_ARGS) return GP_INVALID;
        start = i + 1;
    }
    size_t pointers = (count + 1) * sizeof(char *);
    command->argument_memory = malloc(pointers + (size_t)packed.len);
    if (!command->argument_memory) { *error = ENOMEM; return GP_SYSTEM; }
    command->argv = command->argument_memory;
    char *text = (char *)command->argument_memory + pointers;
    memcpy(text, packed.data, (size_t)packed.len);
    command->argv[0] = text;
    size_t entry = 1;
    for (size_t i = 0; i + 1 < (size_t)packed.len; ++i)
        if (!text[i]) command->argv[entry++] = text + i + 1;
    command->argv[entry] = NULL;
    int result = environment(sets, removes, config->clear_env != 0, command, error);
    if (result) return result;
    if (config->search_path && !strchr(command->argv[0], '/')) {
        command->search = "/bin:/usr/bin";
        for (size_t i = 0; command->envp[i]; ++i) {
            if (!strncmp(command->envp[i], "PATH=", 5)) { command->search = command->envp[i] + 5; break; }
        }
        command->search_length = strnlen(command->search, GP_SEARCH_BYTES + 1);
        if (command->search_length > GP_SEARCH_BYTES) return GP_INVALID;
        size_t components = 1, part = 0, executable = strlen(command->argv[0]);
        for (size_t i = 0; i <= command->search_length; ++i) {
            if (i != command->search_length && command->search[i] != ':') continue;
            size_t length = i - part;
            if ((length && length + 1 + executable > GP_PATH) ||
                (i != command->search_length && ++components > GP_SEARCH_PARTS)) return GP_INVALID;
            part = i + 1;
        }
    }
    return GP_OK;
}

void gp_command_destroy(gp_command *command) {
    free(command->argument_memory); free(command->environment_memory);
    memset(command, 0, sizeof *command);
}

int gp_command_spawn(const gp_command *command, const gp_config *config,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    int64_t deadline, pid_t *pid, int *error) {
    (void)config;
    size_t position = 0;
    bool denied = false;
    do {
        int64_t now = gp_clock_ns();
        if (now < 0) { *error = errno; return GP_SYSTEM; }
        if (now >= deadline) return GP_TIMEOUT;
        char candidate[GP_PATH + 1];
        const char *path = command->argv[0];
        if (command->search) {
            size_t end = position;
            while (end < command->search_length && command->search[end] != ':') ++end;
            size_t component = end - position;
            size_t executable = strlen(command->argv[0]);
            if (component) {
                memcpy(candidate, command->search + position, component);
                candidate[component++] = '/';
            }
            memcpy(candidate + component, command->argv[0], executable + 1);
            path = candidate;
            position = end + 1;
        }
        pid_t attempt = -1;
        int result = posix_spawn(&attempt, path, actions, attributes, command->argv, command->envp);
        if (!result) { *pid = attempt; return GP_OK; }
        if (result == EACCES) denied = true;
        if (!command->search || (result != EACCES && result != ENOENT && result != ENOTDIR)) {
            *error = result; return GP_LAUNCH;
        }
    } while (position <= command->search_length);
    *error = denied ? EACCES : ENOENT;
    return GP_LAUNCH;
}
