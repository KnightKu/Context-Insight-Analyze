#include "nvme_read.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_data_len_arg(const char *arg, uint64_t *out_bytes) {
    if (arg == NULL || out_bytes == NULL || *arg == '\0') {
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
        if (*(endptr + 1) != '\0') {
            return -1;
        }

        switch (*endptr) {
            case 'k':
            case 'K':
                multiplier = 1024ULL;
                break;
            case 'm':
            case 'M':
                multiplier = 1024ULL * 1024ULL;
                break;
            case 'g':
            case 'G':
                multiplier = 1024ULL * 1024ULL * 1024ULL;
                break;
            default:
                return -1;
        }
    }

    if (base == 0ULL) {
        return -1;
    }

    if (base > (ULLONG_MAX / multiplier)) {
        errno = ERANGE;
        return -1;
    }

    *out_bytes = (uint64_t)(base * multiplier);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <device_name> <slba> <data_len[K|M|G]>\n", argv[0]);
        return 1;
    }

    const char *device_name = argv[1];
    char *endptr = NULL;
    errno = 0;
    uint64_t slba = strtoull(argv[2], &endptr, 0);
    if (errno != 0 || endptr == argv[2] || *endptr != '\0') {
        fprintf(stderr, "invalid slba: %s\n", argv[2]);
        return 1;
    }
    uint64_t data_len = 0;
    if (parse_data_len_arg(argv[3], &data_len) != 0) {
        fprintf(stderr, "invalid data_len: %s (examples: 128K, 64M, 1G)\n", argv[3]);
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
