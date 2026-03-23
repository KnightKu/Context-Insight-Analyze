#ifndef NVME_READ_H
#define NVME_READ_H

#include <stdint.h>

int nvme_read(const char *device_name, uint64_t lba, uint32_t data_len, void *buffer);

#endif  // NVME_READ_H
