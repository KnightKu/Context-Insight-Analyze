#include "nvme_read.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <device_name> <filename_prefix>\n", argv[0]);
        return 1;
    }

    const char *device_name = argv[1];
    const char *filename_prefix = argv[2];
    const uint64_t lba = 0ULL;
    const uint64_t data_len = NVME_DEFAULT_DATA_LEN;

    if (nvme_read(device_name, lba, data_len, NULL, filename_prefix) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "nvme passthru read done. file prefix: %s\n", filename_prefix);
    return 0;
}
