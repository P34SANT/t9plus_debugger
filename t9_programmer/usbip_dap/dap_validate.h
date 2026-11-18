#ifndef PLINK_DAP_VALIDATE_H
#define PLINK_DAP_VALIDATE_H
#include <stddef.h>
#include <stdint.h>

/* Validate the whole command before any target-side effects, including writes. */
static inline int dap_request_valid(const uint8_t *data, size_t size)
{
    if (!data || !size || size > 512) return 0;
    size_t minimum = 1;
    switch (data[0]) {
    case 0x00: case 0x02: case 0x13: case 0x17: case 0x18: case 0x1A: case 0x1E:
        minimum = 2; break;
    case 0x01: case 0x09: case 0x1C: minimum = 3; break;
    case 0x04: case 0x08: minimum = 6; break;
    case 0x10: minimum = 7; break;
    case 0x11: case 0x19: minimum = 5; break;
    case 0x12:
        if (size < 2) return 0;
        return size >= 2 + ((data[1] ? data[1] : 256) + 7u) / 8u;
    case 0x05: {
        if (size < 3) return 0;
        size_t pos = 3, response = 3;
        for (unsigned i = 0; i < data[2]; ++i) {
            if (pos >= size) return 0;
            unsigned request = data[pos++];
            if (!(request & 2) || (request & 16)) pos += 4;
            else response += 4;
            if (pos > size || response > 512) return 0;
        }
        return 1;
    }
    case 0x06: {
        if (size < 5) return 0;
        size_t count = data[2] | ((unsigned)data[3] << 8);
        if (data[4] & 2) return count <= 127;
        return count <= 126 && size >= 5 + count * 4;
    }
    case 0x1D: {
        if (size < 2) return 0;
        size_t pos = 2, response = 2;
        for (unsigned i = 0; i < data[1]; ++i) {
            if (pos >= size) return 0;
            unsigned info = data[pos++];
            size_t bytes = (((info & 63) ? (info & 63) : 64) + 7) / 8;
            if (info & 128) response += bytes;
            else pos += bytes;
            if (pos > size || response > 512) return 0;
        }
        return 1;
    }
    default: break;
    }
    return size >= minimum;
}
#endif
