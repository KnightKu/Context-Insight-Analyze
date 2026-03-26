#ifndef NVME_READ_H
#define NVME_READ_H

#include <stdint.h>

#define NVME_READ_CHUNK_BYTES (256ULL * 1024ULL)
#define NVME_SPLIT_BYTES (10ULL * 1024ULL * 1024ULL * 1024ULL)
#define NVME_DEFAULT_DATA_LEN NVME_SPLIT_BYTES
#define NVME_LBA_SIZE_BYTES 512ULL

typedef int (*nvme_read_post_action_t)(void *ctx,
                                       void *data,
                                       uint32_t data_len,
                                       uint64_t offset_bytes);

int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx);

int nvme_read(const char *device_name,
              uint64_t lba,
              uint64_t data_len,
              void *buffer,
              const char *filename);

#endif  // NVME_READ_H
