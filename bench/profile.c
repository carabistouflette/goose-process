/* SPDX-License-Identifier: Apache-2.0 */
/* Native-kernel-only syscall fixture; use the Goose harness for API timing. */
#include "../native/process.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2 && argv[1][0] == '/');
    size_t length = strlen(argv[1]) + 1;
    assert(length <= 4096);
    uint8_t packed[8192]; memcpy(packed, argv[1], length);
    static const char suffix[] = "streams\0" "1048576\0";
    memcpy(packed + length, suffix, sizeof suffix - 1); length += sizeof suffix - 1;
    static uint8_t out[1048576], err[1048576];
    for (int i = 0; i < 3; ++i) {
        int64_t meta[6];
        int result = (int)gp_run((gp_input){packed, (int64_t)length}, (gp_input){NULL, 0},
            5000, 1, (gp_output){out, sizeof out}, (gp_output){err, sizeof err}, (gp_metadata){meta, 6});
        assert(result == 0 && meta[1] == 0 && meta[3] == sizeof out && meta[4] == sizeof err);
    }
    puts("three full capture calls completed");
    return 0;
}
