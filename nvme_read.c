#define _POSIX_C_SOURCE 200809L
#include "nvme_read.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef NVME_POST_ACTION_DEBUG
#define NVME_POST_ACTION_DEBUG 0
#endif

#define NVME_POST_ACTION_RECORD_BYTES_SHORT 8U
#define NVME_POST_ACTION_RECORD_BYTES_LONG 16U

#define NVME_POST_ACTION_OP_READ 0x01U
#define NVME_POST_ACTION_OP_WRITE 0x02U
#define NVME_POST_ACTION_OP_TRIM 0x03U
#define NVME_POST_ACTION_OP_STAT 0x0FU
#define NVME_POST_ACTION_OP_MARKER 0xFFU

#define NVME_POST_ACTION_START_LBA_MASK 0xFFFFFFFFFFULL
#define NVME_POST_ACTION_U24_MASK 0xFFFFFFU
#define NVME_POST_ACTION_U56_MASK 0x00FFFFFFFFFFFFFFULL

// Fast unaligned 64-bit little-endian load.
static inline uint64_t load_le64_u(const unsigned char *p) {
    uint64_t v = 0ULL;
    memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

static int parse_rw_record(uint64_t record_lo,
                           uint64_t record_hi,
                           uint8_t op,
                           uint64_t offset_bytes,
                           uint32_t record_index) {
    uint64_t start_lba = (record_lo >> 8U) & NVME_POST_ACTION_START_LBA_MASK;
    uint16_t length = (uint16_t)(record_lo >> 48U);
    uint16_t reserved = (uint16_t)(record_hi & 0xFFFFU);
    uint32_t latency = (uint32_t)((record_hi >> 16U) & NVME_POST_ACTION_U24_MASK);
    uint32_t time_rel = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid rw reserved: offset=%llu record=%u op=0x%02x reserved=0x%04x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)op,
                (unsigned int)reserved);
        return -1;
    }

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action %s: offset=%llu record=%u start_lba=%llu len=%u latency=%u time=%u\n",
            op == NVME_POST_ACTION_OP_READ ? "read" : "write",
            (unsigned long long)offset_bytes,
            (unsigned int)record_index,
            (unsigned long long)start_lba,
            (unsigned int)length,
            (unsigned int)latency,
            (unsigned int)time_rel);
#else
    (void)op;
    (void)start_lba;
    (void)length;
    (void)latency;
    (void)time_rel;
#endif
    return 0;
}

static int parse_trim_record(uint64_t record_lo,
                             uint64_t record_hi,
                             uint64_t offset_bytes,
                             uint32_t record_index) {
    uint64_t start_lba = (record_lo >> 8U) & NVME_POST_ACTION_START_LBA_MASK;
    uint8_t total_ranges = (uint8_t)(record_lo >> 48U);
    uint8_t range_index = (uint8_t)(record_lo >> 56U);
    uint8_t reserved = (uint8_t)(record_hi & 0xFFU);
    uint32_t length = (uint32_t)((record_hi >> 8U) & 0xFFFFFFFFU);
    uint32_t time_rel = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid trim reserved: offset=%llu record=%u reserved=0x%02x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)reserved);
        return -1;
    }

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action trim: offset=%llu record=%u start_lba=%llu total_ranges=%u range_index=%u "
            "len=%u time=%u\n",
            (unsigned long long)offset_bytes,
            (unsigned int)record_index,
            (unsigned long long)start_lba,
            (unsigned int)total_ranges,
            (unsigned int)range_index,
            (unsigned int)length,
            (unsigned int)time_rel);
#else
    (void)start_lba;
    (void)total_ranges;
    (void)range_index;
    (void)length;
    (void)time_rel;
#endif
    return 0;
}

static int parse_stat_record(uint64_t record_lo,
                             uint64_t offset_bytes,
                             uint32_t record_index) {
    uint8_t reserved = (uint8_t)((record_lo >> 8U) & 0xFFU);
    uint16_t qd = (uint16_t)((record_lo >> 16U) & 0xFFFFU);
    uint8_t wa = (uint8_t)((record_lo >> 32U) & 0xFFU);
    uint32_t time_rel = (uint32_t)((record_lo >> 40U) & NVME_POST_ACTION_U24_MASK);

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid stat reserved: offset=%llu record=%u reserved=0x%02x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)reserved);
        return -1;
    }

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action stat: offset=%llu record=%u qd=%u wa=%u time=%u\n",
            (unsigned long long)offset_bytes,
            (unsigned int)record_index,
            (unsigned int)qd,
            (unsigned int)wa,
            (unsigned int)time_rel);
#else
    (void)qd;
    (void)wa;
    (void)time_rel;
#endif
    return 0;
}

static int parse_marker_record(uint64_t record_lo,
                               uint64_t offset_bytes,
                               uint32_t record_index) {
    uint64_t abs_time = (record_lo >> 8U) & NVME_POST_ACTION_U56_MASK;
#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action marker: offset=%llu record=%u abs_time=%llu\n",
            (unsigned long long)offset_bytes,
            (unsigned int)record_index,
            (unsigned long long)abs_time);
#else
    (void)abs_time;
    (void)offset_bytes;
    (void)record_index;
#endif
    return 0;
}

static int default_post_action(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes) {
    (void)ctx;
    if (data_len == 0U) {
        return 0;
    }
    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (data_len < NVME_POST_ACTION_RECORD_BYTES_SHORT) {
#if NVME_POST_ACTION_DEBUG
        fprintf(stderr,
                "post action debug: no 8-byte unit, offset=%llu data_len=%u\n",
                (unsigned long long)offset_bytes, (unsigned int)data_len);
#endif
        return 0;
    }

    if ((data_len % NVME_POST_ACTION_RECORD_BYTES_SHORT) != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid data_len: offset=%llu data_len=%u not 8-byte aligned\n",
                (unsigned long long)offset_bytes, (unsigned int)data_len);
        return -1;
    }

    const unsigned char *bytes = (const unsigned char *)data;
    // Fast path: decode each record with one/two 64-bit loads, then extract fields via bit ops.
    uint32_t cursor = 0U;
    uint32_t record_index = 0U;
    while (cursor < data_len) {
        const unsigned char *record = bytes + cursor;
        uint64_t record_offset = offset_bytes + (uint64_t)cursor;
        uint64_t record_lo = load_le64_u(record);
        uint8_t op = (uint8_t)(record_lo & 0xFFU);
        int rc = 0;

        if (op == NVME_POST_ACTION_OP_READ || op == NVME_POST_ACTION_OP_WRITE ||
            op == NVME_POST_ACTION_OP_TRIM) {
            if ((data_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
                errno = EINVAL;
                fprintf(stderr,
                        "post action truncated record: offset=%llu record=%u op=0x%02x remain=%u need=%u\n",
                        (unsigned long long)record_offset,
                        (unsigned int)record_index,
                        (unsigned int)op,
                        (unsigned int)(data_len - cursor),
                        (unsigned int)NVME_POST_ACTION_RECORD_BYTES_LONG);
                return -1;
            }
            uint64_t record_hi = load_le64_u(record + 8U);
            if (op == NVME_POST_ACTION_OP_TRIM) {
                rc = parse_trim_record(record_lo, record_hi, record_offset, record_index);
            } else {
                rc = parse_rw_record(record_lo, record_hi, op, record_offset, record_index);
            }
            cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
        } else if (op == NVME_POST_ACTION_OP_STAT) {
            rc = parse_stat_record(record_lo, record_offset, record_index);
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
        } else if (op == NVME_POST_ACTION_OP_MARKER) {
            rc = parse_marker_record(record_lo, record_offset, record_index);
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
        } else {
            errno = EINVAL;
            fprintf(stderr,
                    "post action invalid op: offset=%llu record=%u op=0x%02x\n",
                    (unsigned long long)record_offset,
                    (unsigned int)record_index,
                    (unsigned int)op);
            return -1;
        }
        if (rc != 0) {
            return -1;
        }
        ++record_index;
    }

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

static uint32_t get_sector_size_or_default(int nvme_fd) {
    int logical_block_size = 0;
    if (ioctl(nvme_fd, BLKSSZGET, &logical_block_size) != 0) {
        fprintf(stderr, "BLKSSZGET failed: %s, use default sector_size=%u\n",
                strerror(errno), (unsigned int)NVME_LBA_SIZE_BYTES);
        return (uint32_t)NVME_LBA_SIZE_BYTES;
    }

    if (logical_block_size <= 0) {
        fprintf(stderr, "invalid sector_size=%d, use default sector_size=%u\n",
                logical_block_size, (unsigned int)NVME_LBA_SIZE_BYTES);
        return (uint32_t)NVME_LBA_SIZE_BYTES;
    }

    uint32_t sector_size = (uint32_t)logical_block_size;
    fprintf(stderr, "detected sector_size=%u bytes\n", (unsigned int)sector_size);
    return sector_size;
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

    int nvme_fd = open(device_name, O_RDONLY);
    if (nvme_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device_name, strerror(errno));
        return -1;
    }

    uint32_t sector_size = get_sector_size_or_default(nvme_fd);
    if (sector_size == 0U) {
        errno = EINVAL;
        fprintf(stderr, "invalid sector_size=0\n");
        close(nvme_fd);
        return -1;
    }

    if (data_len % (uint64_t)sector_size != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "data_len must be %u-byte aligned, got %llu\n",
                (unsigned int)sector_size, (unsigned long long)data_len);
        close(nvme_fd);
        return -1;
    }

    if (NVME_READ_CHUNK_BYTES % (uint64_t)sector_size != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "NVME_READ_CHUNK_BYTES must be %u-byte aligned\n",
                (unsigned int)sector_size);
        close(nvme_fd);
        return -1;
    }

    uint64_t read_chunk_bytes = get_mdts_chunk_bytes_or_default(nvme_fd);
    if ((read_chunk_bytes % (uint64_t)sector_size) != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "read chunk must be %u-byte aligned, got %llu\n",
                (unsigned int)sector_size, (unsigned long long)read_chunk_bytes);
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
        uint64_t chunk_lba = offset / (uint64_t)sector_size;
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
        cmd.cdw12 = (uint32_t)(chunk_size / (uint64_t)sector_size) - 1U;
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
