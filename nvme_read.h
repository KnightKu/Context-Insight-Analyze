#ifndef NVME_READ_H
#define NVME_READ_H

#include <stdint.h>

#define NVME_READ_CHUNK_BYTES (256ULL * 1024ULL)
#define NVME_SPLIT_BYTES (10ULL * 1024ULL * 1024ULL * 1024ULL)
#define NVME_MAX_THREADS 32U
#define NVME_DEFAULT_DATA_LEN NVME_SPLIT_BYTES
#define NVME_LBA_SIZE_BYTES 512ULL

int nvme_read(const char *device_name,
              uint64_t lba,
              uint64_t data_len,
              void *buffer,
              const char *filename);

#endif  // NVME_READ_H
