#include "nvme_read.h"

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <device_name> <filename_prefix> [backup_lba]\n", argv[0]);
        return 1;
    }

    const char *device_name = argv[1];
    const char *filename_prefix = argv[2];
    uint64_t backup_lba = 0ULL;
    if (argc == 4) {
        char *endptr = NULL;
        errno = 0;
        backup_lba = strtoull(argv[3], &endptr, 0);
        if (errno != 0 || endptr == argv[3] || *endptr != '\0') {
            fprintf(stderr, "invalid backup_lba: %s\n", argv[3]);
            return 1;
        }
    }
    const uint64_t data_len = NVME_DEFAULT_DATA_LEN;

    if (nvme_read(device_name, backup_lba, data_len, NULL, filename_prefix) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "nvme passthru read done. file prefix: %s\n", filename_prefix);
    return 0;
}
