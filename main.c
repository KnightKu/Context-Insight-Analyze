#include "nvme_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <device_name>\n", argv[0]);
        return 1;
    }

    const char *device_name = argv[1];

    // 保持与原命令一致的数据长度和 LBA。
    const uint32_t data_len = 4096U;
    const uint64_t lba = 0x0000000180000000ULL;  // cdw11=1, cdw10=2147483648

    void *buffer = NULL;
    if (posix_memalign(&buffer, 4096, data_len) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        return 1;
    }
    memset(buffer, 0, data_len);

    if (nvme_read(device_name, lba, data_len, buffer) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        free(buffer);
        return 1;
    }

    size_t written = fwrite(buffer, 1, data_len, stdout);
    if (written != data_len) {
        fprintf(stderr, "fwrite failed: wrote %zu of %u bytes\n", written, data_len);
        free(buffer);
        return 1;
    }

    fprintf(stderr,
            "nvme passthru read done.\n"
            "SLBA      : 0x%016" PRIx64 "\n"
            "BackupLBA : 0x%016" PRIx64 "\n",
            lba, lba);

    free(buffer);
    return 0;
}
