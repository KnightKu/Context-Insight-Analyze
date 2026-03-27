#include "nvme_read.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <device_name> <output_filename>\n", argv[0]);
        return 1;
    }

    const char *device_name = argv[1];
    const char *output_filename = argv[2];
    const uint64_t lba = 0ULL;
    const uint64_t data_len = NVME_DEFAULT_DATA_LEN;

    if (nvme_read(device_name, lba, data_len, NULL, output_filename) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "nvme passthru read done. output: %s\n", output_filename);
    return 0;
}
