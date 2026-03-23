#include "nvme_read.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    const char *device_name;
    const char *filename_prefix;
    uint64_t backup_lba_base;
    uint64_t segment_offset_bytes;
    uint64_t segment_len_bytes;
    size_t segment_index;
    int rc;
} nvme_thread_arg_t;

static int write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t written = 0;
    while (written < count) {
        ssize_t n = write(fd, p + written, count - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static void *read_segment_worker(void *arg_ptr) {
    nvme_thread_arg_t *arg = (nvme_thread_arg_t *)arg_ptr;
    arg->rc = -1;

    int nvme_fd = open(arg->device_name, O_RDONLY);
    if (nvme_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", arg->device_name, strerror(errno));
        return NULL;
    }

    char output_name[4096];
    int name_len = snprintf(output_name, sizeof(output_name), "%s_part%04zu.bin",
                            arg->filename_prefix, arg->segment_index);
    if (name_len < 0 || (size_t)name_len >= sizeof(output_name)) {
        fprintf(stderr, "output filename too long for segment %zu\n", arg->segment_index);
        close(nvme_fd);
        return NULL;
    }

    int out_fd = open(output_name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", output_name, strerror(errno));
        close(nvme_fd);
        return NULL;
    }

    void *chunk_buf = NULL;
    if (posix_memalign(&chunk_buf, 4096, NVME_READ_CHUNK_BYTES) != 0) {
        fprintf(stderr, "posix_memalign failed for segment %zu\n", arg->segment_index);
        close(out_fd);
        close(nvme_fd);
        return NULL;
    }

    uint64_t offset = 0;
    while (offset < arg->segment_len_bytes) {
        uint64_t remaining = arg->segment_len_bytes - offset;
        uint64_t chunk_size = remaining > NVME_READ_CHUNK_BYTES ? NVME_READ_CHUNK_BYTES : remaining;
        uint64_t chunk_lba =
            (arg->segment_offset_bytes + offset) / NVME_LBA_SIZE_BYTES;
        uint64_t backup_lba = arg->backup_lba_base + chunk_lba;

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
            fprintf(stderr, "ioctl failed in segment %zu: %s\n", arg->segment_index, strerror(errno));
            free(chunk_buf);
            close(out_fd);
            close(nvme_fd);
            return NULL;
        }

        if (write_all(out_fd, chunk_buf, (size_t)chunk_size) != 0) {
            fprintf(stderr, "write failed for %s: %s\n", output_name, strerror(errno));
            free(chunk_buf);
            close(out_fd);
            close(nvme_fd);
            return NULL;
        }

        offset += chunk_size;
    }

    free(chunk_buf);
    close(out_fd);
    close(nvme_fd);
    arg->rc = 0;
    return NULL;
}

int nvme_read(const char *device_name,
              uint64_t lba,
              uint64_t data_len,
              void *buffer,
              const char *filename) {
    (void)buffer;

    if (device_name == NULL || filename == NULL || data_len == 0) {
        errno = EINVAL;
        fprintf(stderr, "invalid argument: device_name/filename/data_len\n");
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

    uint64_t segment_count_u64 =
        (data_len + NVME_SPLIT_BYTES - 1ULL) / NVME_SPLIT_BYTES;
    if (segment_count_u64 == 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "invalid segment count for data_len=%llu\n", (unsigned long long)data_len);
        return -1;
    }

    size_t segment_count = (size_t)segment_count_u64;
    size_t start = 0;
    while (start < segment_count) {
        size_t batch = segment_count - start;
        if (batch > NVME_MAX_THREADS) {
            batch = NVME_MAX_THREADS;
        }

        pthread_t threads[NVME_MAX_THREADS];
        nvme_thread_arg_t args[NVME_MAX_THREADS];
        memset(threads, 0, sizeof(threads));
        memset(args, 0, sizeof(args));

        for (size_t i = 0; i < batch; ++i) {
            size_t seg_idx = start + i;
            uint64_t seg_offset = (uint64_t)seg_idx * NVME_SPLIT_BYTES;
            uint64_t seg_len = data_len - seg_offset;
            if (seg_len > NVME_SPLIT_BYTES) {
                seg_len = NVME_SPLIT_BYTES;
            }

            args[i].device_name = device_name;
            args[i].filename_prefix = filename;
            // cdw10/cdw11 默认从 0 LBA 开始递增。
            args[i].backup_lba_base = lba;
            args[i].segment_offset_bytes = seg_offset;
            args[i].segment_len_bytes = seg_len;
            args[i].segment_index = seg_idx;
            args[i].rc = -1;

            int rc = pthread_create(&threads[i], NULL, read_segment_worker, &args[i]);
            if (rc != 0) {
                errno = rc;
                fprintf(stderr, "pthread_create failed for segment %zu: %s\n",
                        seg_idx, strerror(errno));
                for (size_t j = 0; j < i; ++j) {
                    pthread_join(threads[j], NULL);
                }
                return -1;
            }
        }

        int has_error = 0;
        for (size_t i = 0; i < batch; ++i) {
            pthread_join(threads[i], NULL);
            if (args[i].rc != 0) {
                has_error = 1;
            }
        }

        if (has_error) {
            errno = EIO;
            return -1;
        }

        start += batch;
    }

    return 0;
}
