#define _POSIX_C_SOURCE 200809L
#include "nvme_read.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int default_post_action(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes) {
    (void)ctx;
    (void)data;
    (void)data_len;
    (void)offset_bytes;
    return 0;
}

static nvme_read_post_action_t g_post_action = default_post_action;
static void *g_post_action_ctx = NULL;

int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx) {
    if (action == NULL) {
        g_post_action = default_post_action;
        g_post_action_ctx = NULL;
        return 0;
    }

    g_post_action = action;
    g_post_action_ctx = ctx;
    return 0;
}

static uint64_t get_mdts_chunk_bytes_or_default(int nvme_fd) {
    unsigned char *id_ctrl = NULL;
    if (posix_memalign((void **)&id_ctrl, 4096, 4096) != 0) {
        fprintf(stderr, "posix_memalign failed for identify buffer, fallback chunk=%llu\n",
                (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }
    memset(id_ctrl, 0, 4096);

    struct nvme_admin_cmd admin_cmd;
    memset(&admin_cmd, 0, sizeof(admin_cmd));
    admin_cmd.opcode = 0x06;  // Identify
    admin_cmd.nsid = 0;
    admin_cmd.addr = (uint64_t)(uintptr_t)id_ctrl;
    admin_cmd.data_len = 4096;
    admin_cmd.cdw10 = 1;      // CNS = 1, Identify Controller

    if (ioctl(nvme_fd, NVME_IOCTL_ADMIN_CMD, &admin_cmd) < 0) {
        fprintf(stderr, "identify controller failed: %s, fallback chunk=%llu\n",
                strerror(errno), (unsigned long long)NVME_READ_CHUNK_BYTES);
        free(id_ctrl);
        return NVME_READ_CHUNK_BYTES;
    }

    // Identify Controller data structure: byte 77 is MDTS.
    uint8_t mdts = id_ctrl[77];
    free(id_ctrl);

    if (mdts == 0U) {
        // 0 means no MDTS limit reported, keep using configured default chunk.
        fprintf(stderr, "mdts=0 (no limit reported), use fallback chunk=%llu\n",
                (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    if (mdts >= 52U) {
        fprintf(stderr, "mdts=%u too large, fallback chunk=%llu\n",
                (unsigned int)mdts, (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    uint64_t chunk_bytes = (1ULL << (12U + (uint64_t)mdts));
    if (chunk_bytes < NVME_LBA_SIZE_BYTES || (chunk_bytes % NVME_LBA_SIZE_BYTES) != 0ULL) {
        fprintf(stderr, "invalid mdts-derived chunk=%llu, fallback chunk=%llu\n",
                (unsigned long long)chunk_bytes, (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    if (chunk_bytes > (uint64_t)UINT32_MAX) {
        fprintf(stderr, "mdts-derived chunk too large=%llu, fallback chunk=%llu\n",
                (unsigned long long)chunk_bytes, (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    fprintf(stderr, "mdts=%u, read chunk=%llu bytes\n",
            (unsigned int)mdts, (unsigned long long)chunk_bytes);
    return chunk_bytes;
}

int nvme_read(const char *device_name,
              uint64_t slba,
              uint64_t data_len,
              void *buffer) {
    (void)buffer;

    if (device_name == NULL || data_len == 0) {
        errno = EINVAL;
        fprintf(stderr, "invalid argument: device_name/data_len\n");
        return -1;
    }

    if (data_len % NVME_LBA_SIZE_BYTES != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "data_len must be %llu-byte aligned, got %llu\n",
                (unsigned long long)NVME_LBA_SIZE_BYTES, (unsigned long long)data_len);
        return -1;
    }

    if (NVME_READ_CHUNK_BYTES % NVME_LBA_SIZE_BYTES != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "NVME_READ_CHUNK_BYTES must be %llu-byte aligned\n",
                (unsigned long long)NVME_LBA_SIZE_BYTES);
        return -1;
    }

    int nvme_fd = open(device_name, O_RDONLY);
    if (nvme_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device_name, strerror(errno));
        return -1;
    }

    uint64_t read_chunk_bytes = get_mdts_chunk_bytes_or_default(nvme_fd);
    if ((read_chunk_bytes % NVME_LBA_SIZE_BYTES) != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "read chunk must be %llu-byte aligned, got %llu\n",
                (unsigned long long)NVME_LBA_SIZE_BYTES, (unsigned long long)read_chunk_bytes);
        close(nvme_fd);
        return -1;
    }

    void *chunk_buf = NULL;
    if (posix_memalign(&chunk_buf, 4096, (size_t)read_chunk_bytes) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        close(nvme_fd);
        return -1;
    }

    struct timespec ts_begin;
    if (clock_gettime(CLOCK_MONOTONIC, &ts_begin) != 0) {
        fprintf(stderr, "clock_gettime begin failed: %s\n", strerror(errno));
        free(chunk_buf);
        close(nvme_fd);
        return -1;
    }

    uint64_t offset = 0;
    uint64_t total_read_bytes = 0;
    while (offset < data_len) {
        uint64_t remaining = data_len - offset;
        uint64_t chunk_size = remaining > read_chunk_bytes ? read_chunk_bytes : remaining;
        // Real LBA starts from 0 and increases with read offset.
        uint64_t chunk_lba = offset / NVME_LBA_SIZE_BYTES;
        // slba is encoded into cdw14/cdw15 after conversion.
        uint64_t backup_lba = slba + chunk_lba;

        struct nvme_passthru_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = 0x02;      // NVM Read
        cmd.nsid = 1;
        cmd.addr = (uint64_t)(uintptr_t)chunk_buf;
        cmd.data_len = (uint32_t)chunk_size;
        cmd.cdw10 = (uint32_t)(chunk_lba & 0xFFFFFFFFULL);
        cmd.cdw11 = (uint32_t)((chunk_lba >> 32) & 0xFFFFFFFFULL);
        cmd.cdw12 = (uint32_t)(chunk_size / NVME_LBA_SIZE_BYTES) - 1U;
        cmd.cdw14 = (uint32_t)(backup_lba & 0xFFFFFFFFULL);
        cmd.cdw15 = (uint32_t)((backup_lba >> 32) & 0xFFFFFFFFULL);

        if (ioctl(nvme_fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
            fprintf(stderr, "ioctl failed at offset=%llu: %s\n",
                    (unsigned long long)offset, strerror(errno));
            free(chunk_buf);
            close(nvme_fd);
            return -1;
        }

        if (g_post_action(g_post_action_ctx, chunk_buf, (uint32_t)chunk_size, offset) != 0) {
            if (errno == 0) {
                errno = EIO;
            }
            fprintf(stderr, "post action failed at offset=%llu: %s\n",
                    (unsigned long long)offset, strerror(errno));
            free(chunk_buf);
            close(nvme_fd);
            return -1;
        }

        offset += chunk_size;
        total_read_bytes += chunk_size;
    }

    struct timespec ts_end;
    if (clock_gettime(CLOCK_MONOTONIC, &ts_end) == 0) {
        double elapsed_s = (double)(ts_end.tv_sec - ts_begin.tv_sec) +
                           (double)(ts_end.tv_nsec - ts_begin.tv_nsec) / 1000000000.0;
        if (elapsed_s <= 0.0) {
            elapsed_s = 1e-9;
        }
        double bandwidth_mib_s =
            ((double)total_read_bytes / (1024.0 * 1024.0)) / elapsed_s;
        fprintf(stderr,
                "read stats: bytes=%llu elapsed=%.6f sec bandwidth=%.2f MiB/s\n",
                (unsigned long long)total_read_bytes, elapsed_s, bandwidth_mib_s);
    }

    free(chunk_buf);
    close(nvme_fd);
    return 0;
}
