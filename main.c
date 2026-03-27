#include "nvme_read.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u64_with_unit(const char *arg, uint64_t *out_value) {
    if (arg == NULL || out_value == NULL || *arg == '\0') {
        return -1;
    }

    char *endptr = NULL;
    errno = 0;
    unsigned long long base = strtoull(arg, &endptr, 10);
    if (errno != 0 || endptr == arg) {
        return -1;
    }

    uint64_t multiplier = 1ULL;
    if (*endptr != '\0') {
        char unit = (char)toupper((unsigned char)*endptr);
        switch (unit) {
            case 'K':
                multiplier = 1024ULL;
                break;
            case 'M':
                multiplier = 1024ULL * 1024ULL;
                break;
            case 'G':
                multiplier = 1024ULL * 1024ULL * 1024ULL;
                break;
            case 'T':
                multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
                break;
            default:
                return -1;
        }

        ++endptr;
        if (*endptr == 'B' || *endptr == 'b') {
            ++endptr;
        }
        if (*endptr != '\0') {
            return -1;
        }
    }

    if (base > (ULLONG_MAX / multiplier)) {
        errno = ERANGE;
        return -1;
    }

    *out_value = (uint64_t)base * multiplier;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <device_name> <slba[K|M|G|T]> <data_len[K|M|G|T]>\n", argv[0]);
        return 1;
    }

    const char *device_name = argv[1];
    uint64_t slba = 0ULL;
    if (parse_u64_with_unit(argv[2], &slba) != 0) {
        fprintf(stderr, "invalid slba: %s\n", argv[2]);
        return 1;
    }

    uint64_t data_len = 0;
    if (parse_u64_with_unit(argv[3], &data_len) != 0 || data_len == 0ULL) {
        fprintf(stderr, "invalid data_len: %s (examples: 128K, 64M, 1G, 1T)\n", argv[3]);
        return 1;
    }

    if (nvme_read(device_name, slba, data_len, NULL) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "nvme passthru read done. slba=%" PRIu64 " data_len=%" PRIu64 "\n",
            slba, data_len);
    return 0;
}
