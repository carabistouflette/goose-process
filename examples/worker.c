/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed framed echo worker: u32 big-endian length, <=4096 binary bytes. */
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
static int receive(uint8_t *data, size_t length, int allow_eof) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(0, data + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) return allow_eof && offset == 0 ? 0 : -1;
        if (count < 0) return -1;
        offset += (size_t)count;
    }
    return 1;
}
static int emit(int fd, const uint8_t *data, size_t length) {
    while (length) {
        ssize_t count = write(fd, data, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        data += count; length -= (size_t)count;
    }
    return 0;
}
int main(void) {
    uint8_t header[4], payload[4096];
    static const uint8_t ack[4] = {'a', 'c', 'k', 0};
    for (;;) {
        int result = receive(header, sizeof header, 1);
        if (!result) return 0;
        if (result < 0) return 2;
        uint32_t length = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                          ((uint32_t)header[2] << 8) | header[3];
        if (length > sizeof payload || receive(payload, length, 0) < 0) return 2;
        if (emit(1, header, sizeof header) || emit(1, payload, length) || emit(2, ack, sizeof ack)) return 3;
    }
}
