#include "nvme_read.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int nvme_read(const char *device_name, uint64_t lba, uint32_t data_len, void *buffer) {
    if (device_name == NULL || buffer == NULL || data_len == 0) {
        errno = EINVAL;
        fprintf(stderr, "invalid argument: device_name/buffer/data_len\n");
        return -1;
    }

    if (data_len % 512U != 0U) {
        errno = EINVAL;
        fprintf(stderr, "data_len must be 512-byte aligned, got %u\n", data_len);
        return -1;
    }

    uint32_t block_count = data_len / 512U;
    if (block_count == 0U || block_count > 65536U) {
        errno = EINVAL;
        fprintf(stderr, "invalid block count from data_len=%u\n", data_len);
        return -1;
    }

    int fd = open(device_name, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device_name, strerror(errno));
        return -1;
    }

    struct nvme_passthru_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.opcode = 0x02;                      // NVM Read
    cmd.nsid = 1;                           // 与原命令一致
    cmd.addr = (uint64_t)(uintptr_t)buffer;
    cmd.data_len = data_len;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFFULL);         // SLBA低32位
    cmd.cdw11 = (uint32_t)((lba >> 32) & 0xFFFFFFFFULL); // SLBA高32位
    cmd.cdw12 = block_count - 1U;                         // NLB = block_count - 1

    // 备份 LBA，放在 cdw14/cdw15。
    cmd.cdw14 = (uint32_t)(lba & 0xFFFFFFFFULL);
    cmd.cdw15 = (uint32_t)((lba >> 32) & 0xFFFFFFFFULL);

    if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
        fprintf(stderr, "ioctl NVME_IOCTL_IO_CMD failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}
