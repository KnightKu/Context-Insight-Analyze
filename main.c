#include "nvme_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <device_name> <slba>\n", argv[0]);
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
    const uint64_t data_len = NVME_DEFAULT_DATA_LEN;

    if (nvme_read(device_name, slba, data_len, NULL) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "nvme passthru read done. slba=%" PRIu64 "\n", slba);
    return 0;
}
