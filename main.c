#include "nvme_read.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int parse_u64_with_unit(const char *text, uint64_t *value) {
    if (text == NULL || *text == '\0' || value == NULL) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    char *endptr = NULL;
    unsigned long long base = strtoull(text, &endptr, 10);
    if (errno != 0 || endptr == text) {
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
                errno = EINVAL;
                return -1;
        }

        ++endptr;
        if (*endptr == 'B' || *endptr == 'b') {
            ++endptr;
        }
        if (*endptr != '\0') {
            errno = EINVAL;
            return -1;
        }
    }

    if ((uint64_t)base > UINT64_MAX / multiplier) {
        errno = ERANGE;
        return -1;
    }

    *value = (uint64_t)base * multiplier;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 5) {
        fprintf(stderr,
                "usage: %s <device_name> <output_filename> [slba] [data_len]\n"
                "  slba/data_len support optional units: K M G T (e.g. 64K, 1G)\n",
                argv[0]);
        return 1;
    }

    const char *device_name = argv[1];
    const char *output_filename = argv[2];
    uint64_t lba = 0ULL;
    uint64_t data_len = NVME_DEFAULT_DATA_LEN;

    if (argc == 5) {
        if (parse_u64_with_unit(argv[3], &lba) != 0) {
            fprintf(stderr, "invalid slba: %s\n", argv[3]);
            return 1;
        }
        if (parse_u64_with_unit(argv[4], &data_len) != 0) {
            fprintf(stderr, "invalid data_len: %s\n", argv[4]);
            return 1;
        }
    }

    if (nvme_read(device_name, lba, data_len, NULL, output_filename) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "nvme passthru read done. output: %s\n", output_filename);
    return 0;
}
