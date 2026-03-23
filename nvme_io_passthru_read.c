// 等价于以下命令：
// nvme io-passthru /dev/nvme0n1
//   --opcode=0x02
//   --namespace-id=1
//   --data-len=4096
//   --read
//   --cdw10=2147483648
//   --cdw11=1
//   --cdw12=7
//   --cdw14=0x12345678
//   --cdw15=0x0
//   -b
//
// 编译：
//   gcc -O2 -Wall -Wextra -o nvme_io_passthru_read nvme_io_passthru_read.c
//
// 运行：
//   ./nvme_io_passthru_read [设备路径] > out.bin

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    const char *dev_path = "/dev/nvme0n1";
    if (argc > 1) {
        dev_path = argv[1];
    }

    const uint32_t data_len = 4096;
    void *data = NULL;

    // 4K 对齐缓冲区，便于 DMA 访问。
    if (posix_memalign(&data, 4096, data_len) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        return 1;
    }
    memset(data, 0, data_len);

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
        free(data);
        return 1;
    }

    struct nvme_passthru_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.opcode = 0x02;         // NVM Read
    cmd.nsid = 1;              // --namespace-id=1
    cmd.addr = (uint64_t)(uintptr_t)data;
    cmd.data_len = data_len;   // --data-len=4096
    cmd.cdw10 = 2147483648U;   // --cdw10=2147483648 (SLBA低32位)
    cmd.cdw11 = 1;             // --cdw11=1          (SLBA高32位)
    cmd.cdw12 = 7;             // --cdw12=7          (NLB=7 => 8 blocks)

    // 备份 LBA（按你的要求，放在 cdw14/cdw15）
    cmd.cdw14 = 0x12345678U;   // --cdw14=0x12345678 (备份LBA低32位)
    cmd.cdw15 = 0x0U;          // --cdw15=0x0        (备份LBA高32位)

    if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
        fprintf(stderr, "ioctl NVME_IOCTL_IO_CMD failed: %s\n", strerror(errno));
        close(fd);
        free(data);
        return 1;
    }

    // 对应 nvme-cli 的 -b：把读回的数据按二进制直接输出到 stdout。
    size_t written = fwrite(data, 1, data_len, stdout);
    if (written != data_len) {
        fprintf(stderr, "fwrite failed: wrote %zu of %u bytes\n", written, data_len);
        close(fd);
        free(data);
        return 1;
    }

    fprintf(stderr,
            "nvme passthru read done.\n"
            "SLBA      : 0x%08" PRIx32 "%08" PRIx32 "\n"
            "BackupLBA : 0x%08" PRIx32 "%08" PRIx32 "\n"
            "Result    : 0x%x\n",
            cmd.cdw11, cmd.cdw10, cmd.cdw15, cmd.cdw14, cmd.result);

    close(fd);
    free(data);
    return 0;
}
