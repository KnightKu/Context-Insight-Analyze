#define _POSIX_C_SOURCE 200809L

#include "post_action.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "post_action_latency.h"
#include "post_action_stats.h"

#ifndef NVME_POST_ACTION_DEBUG
#define NVME_POST_ACTION_DEBUG 0
#endif

#ifndef NVME_POST_ACTION_SKIP_STAT
#define NVME_POST_ACTION_SKIP_STAT 0
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
#define NVME_POST_ACTION_TRIM_MAX_RANGES 256U

typedef struct {
    uint8_t op;
    uint64_t offset_bytes;
    uint32_t record_index;
} nvme_post_action_record_meta_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint64_t start_lba;
    uint16_t length;
    uint32_t latency;
    uint32_t time_rel;
} nvme_post_action_rw_t;

typedef struct {
    uint64_t start_lba;
    uint8_t range_index;
    uint32_t length;
    uint32_t time_rel;
} nvme_post_action_trim_range_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint16_t total_ranges;
    uint16_t range_count;
    nvme_post_action_trim_range_t ranges[NVME_POST_ACTION_TRIM_MAX_RANGES];
} nvme_post_action_trim_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint16_t qd;
    uint8_t wa;
    uint32_t time_rel;
} nvme_post_action_stat_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint64_t abs_time;
} nvme_post_action_marker_t;

typedef struct {
    uint64_t start_lba;
    uint8_t total_ranges;
    uint8_t range_index;
    uint32_t length;
    uint32_t time_rel;
} nvme_post_action_trim_record_fields_t;

typedef struct {
    uint64_t marker_abs_time_us;
    int has_marker;
} nvme_post_action_time_ref_t;

static pthread_mutex_t g_post_action_invalid_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_post_action_invalid_records = 0ULL;

static int default_post_action(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes);
static nvme_read_post_action_t g_post_action = default_post_action;
static void *g_post_action_ctx = NULL;
static int g_post_action_debug_enabled = 0;

void nvme_post_action_reset_invalid_count(void) {
    pthread_mutex_lock(&g_post_action_invalid_mutex);
    g_post_action_invalid_records = 0ULL;
    pthread_mutex_unlock(&g_post_action_invalid_mutex);
}

static void post_action_add_invalid_count(uint64_t delta) {
    if (delta == 0ULL) {
        return;
    }
    pthread_mutex_lock(&g_post_action_invalid_mutex);
    g_post_action_invalid_records += delta;
    pthread_mutex_unlock(&g_post_action_invalid_mutex);
}

uint64_t nvme_post_action_get_invalid_count(void) {
    pthread_mutex_lock(&g_post_action_invalid_mutex);
    uint64_t v = g_post_action_invalid_records;
    pthread_mutex_unlock(&g_post_action_invalid_mutex);
    return v;
}

static int resolve_abs_time_us(uint32_t time_rel_us,
                               nvme_post_action_time_ref_t *time_ref,
                               uint64_t *abs_time_us_out) {
    if (time_ref == NULL || abs_time_us_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (time_ref->has_marker == 0) {
        errno = EINVAL;
        return -1;
    }
    *abs_time_us_out = time_ref->marker_abs_time_us + (uint64_t)time_rel_us;
    return 0;
}

static inline uint64_t load_le64_u(const unsigned char *p) {
    uint64_t v = 0ULL;
    memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

static uint16_t trim_total_ranges_from_raw(uint8_t raw_total_ranges) {
    return raw_total_ranges == 0U ? NVME_POST_ACTION_TRIM_MAX_RANGES : (uint16_t)raw_total_ranges;
}

static int parse_rw_record(uint64_t record_lo,
                           uint64_t record_hi,
                           uint8_t op,
                           uint64_t offset_bytes,
                           uint32_t record_index,
                           nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_rw_t parsed;
    parsed.meta.op = op;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    parsed.start_lba = (record_lo >> 8U) & NVME_POST_ACTION_START_LBA_MASK;
    parsed.length = (uint16_t)(record_lo >> 48U);
    uint16_t reserved = (uint16_t)(record_hi & 0xFFFFU);
    parsed.latency = (uint32_t)((record_hi >> 16U) & NVME_POST_ACTION_U24_MASK);
    parsed.time_rel = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);
    uint64_t abs_time_us = 0ULL;

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid rw reserved: offset=%llu record=%u op=0x%02x reserved=0x%04x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)op,
                (unsigned int)reserved);
        return -1;
    }
    if (resolve_abs_time_us(parsed.time_rel, time_ref, &abs_time_us) != 0) {
        fprintf(stderr,
                "post action missing marker for rw time: offset=%llu record=%u op=0x%02x time_rel=%u\n",
                (unsigned long long)offset_bytes,
                (unsigned int)record_index,
                (unsigned int)op,
                (unsigned int)parsed.time_rel);
        return -1;
    }

    if (op == NVME_POST_ACTION_OP_WRITE) {
        nvme_post_action_stats_update_write(parsed.start_lba, (uint64_t)parsed.length, abs_time_us);
        nvme_post_action_latency_record_write(parsed.latency);
    } else if (op == NVME_POST_ACTION_OP_READ) {
        nvme_post_action_stats_update_read(parsed.start_lba, (uint64_t)parsed.length, abs_time_us);
        nvme_post_action_latency_record_read(parsed.latency);
    }

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action %s: offset=%llu record=%u start_lba=%llu len=%u latency=%u time_rel=%u abs_time_us=%llu\n",
            parsed.meta.op == NVME_POST_ACTION_OP_READ ? "read" : "write",
            (unsigned long long)parsed.meta.offset_bytes,
            (unsigned int)parsed.meta.record_index,
            (unsigned long long)parsed.start_lba,
            (unsigned int)parsed.length,
            (unsigned int)parsed.latency,
            (unsigned int)parsed.time_rel,
            (unsigned long long)abs_time_us);
#else
    (void)parsed;
    (void)abs_time_us;
#endif
    return 0;
}

static int decode_trim_record_fields(uint64_t record_lo,
                                     uint64_t record_hi,
                                     uint64_t offset_bytes,
                                     uint32_t record_index,
                                     nvme_post_action_trim_record_fields_t *fields) {
    if (fields == NULL) {
        errno = EINVAL;
        return -1;
    }
    fields->start_lba = (record_lo >> 8U) & NVME_POST_ACTION_START_LBA_MASK;
    fields->total_ranges = (uint8_t)(record_lo >> 48U);
    fields->range_index = (uint8_t)(record_lo >> 56U);
    uint8_t reserved = (uint8_t)(record_hi & 0xFFU);
    fields->length = (uint32_t)((record_hi >> 8U) & 0xFFFFFFFFU);
    fields->time_rel = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid trim reserved: offset=%llu record=%u reserved=0x%02x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)reserved);
        return -1;
    }
    return 0;
}

static int parse_trim_group(const unsigned char *bytes,
                            uint32_t data_len,
                            uint32_t cursor,
                            uint64_t base_offset_bytes,
                            uint32_t record_index,
                            nvme_post_action_time_ref_t *time_ref,
                            uint32_t *consumed_bytes,
                            uint32_t *consumed_records) {
    if (bytes == NULL || consumed_bytes == NULL || consumed_records == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (time_ref == NULL || time_ref->has_marker == 0) {
        errno = EINVAL;
        fprintf(stderr,
                "post action missing marker for trim time: offset=%llu record=%u\n",
                (unsigned long long)(base_offset_bytes + (uint64_t)cursor),
                (unsigned int)record_index);
        return -1;
    }
    if ((data_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
        errno = EINVAL;
        return -1;
    }

    nvme_post_action_trim_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.meta.op = NVME_POST_ACTION_OP_TRIM;
    parsed.meta.offset_bytes = base_offset_bytes + (uint64_t)cursor;
    parsed.meta.record_index = record_index;

    uint64_t first_lo = load_le64_u(bytes + cursor);
    uint64_t first_hi = load_le64_u(bytes + cursor + 8U);
    if (((uint8_t)(first_lo & 0xFFU)) != NVME_POST_ACTION_OP_TRIM) {
        errno = EINVAL;
        return -1;
    }

    nvme_post_action_trim_record_fields_t first_fields;
    if (decode_trim_record_fields(first_lo, first_hi, parsed.meta.offset_bytes, parsed.meta.record_index,
                                  &first_fields) != 0) {
        return -1;
    }

    parsed.total_ranges = trim_total_ranges_from_raw(first_fields.total_ranges);
    if (parsed.total_ranges == 0U || parsed.total_ranges > NVME_POST_ACTION_TRIM_MAX_RANGES) {
        errno = EINVAL;
        return -1;
    }

    for (uint16_t i = 0; i < parsed.total_ranges; ++i) {
        uint32_t group_cursor = cursor + ((uint32_t)i * NVME_POST_ACTION_RECORD_BYTES_LONG);
        if ((data_len - group_cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
            errno = EINVAL;
            return -1;
        }
        uint64_t record_lo = load_le64_u(bytes + group_cursor);
        if ((uint8_t)(record_lo & 0xFFU) != NVME_POST_ACTION_OP_TRIM) {
            errno = EINVAL;
            return -1;
        }
        uint64_t record_hi = load_le64_u(bytes + group_cursor + 8U);
        nvme_post_action_trim_record_fields_t fields;
        if (decode_trim_record_fields(record_lo, record_hi,
                                      base_offset_bytes + (uint64_t)group_cursor,
                                      record_index + (uint32_t)i, &fields) != 0) {
            return -1;
        }
        if (trim_total_ranges_from_raw(fields.total_ranges) != parsed.total_ranges) {
            errno = EINVAL;
            return -1;
        }

        parsed.ranges[i].start_lba = fields.start_lba;
        parsed.ranges[i].range_index = fields.range_index;
        parsed.ranges[i].length = fields.length;
        parsed.ranges[i].time_rel = fields.time_rel;
        uint64_t abs_time_us = 0ULL;
        if (resolve_abs_time_us(fields.time_rel, time_ref, &abs_time_us) != 0) {
            return -1;
        }
        nvme_post_action_stats_update_write(fields.start_lba, (uint64_t)fields.length, abs_time_us);
    }
    parsed.range_count = parsed.total_ranges;
    (void)parsed;
    *consumed_bytes = (uint32_t)parsed.total_ranges * NVME_POST_ACTION_RECORD_BYTES_LONG;
    *consumed_records = (uint32_t)parsed.total_ranges;
    return 0;
}

#if !NVME_POST_ACTION_SKIP_STAT
static int parse_stat_record(uint64_t record_lo,
                             uint64_t offset_bytes,
                             uint32_t record_index,
                             nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_stat_t parsed;
    parsed.meta.op = NVME_POST_ACTION_OP_STAT;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    uint8_t reserved = (uint8_t)((record_lo >> 8U) & 0xFFU);
    parsed.qd = (uint16_t)((record_lo >> 16U) & 0xFFFFU);
    parsed.wa = (uint8_t)((record_lo >> 32U) & 0xFFU);
    parsed.time_rel = (uint32_t)((record_lo >> 40U) & NVME_POST_ACTION_U24_MASK);
    uint64_t abs_time_us = 0ULL;
    if (reserved != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_abs_time_us(parsed.time_rel, time_ref, &abs_time_us) != 0) {
        return -1;
    }
    (void)abs_time_us;
    return 0;
}
#endif

static int parse_marker_record(uint64_t record_lo,
                               uint64_t offset_bytes,
                               uint32_t record_index,
                               nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_marker_t parsed;
    parsed.meta.op = NVME_POST_ACTION_OP_MARKER;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    parsed.abs_time = (record_lo >> 8U) & NVME_POST_ACTION_U56_MASK;
    if (time_ref == NULL) {
        errno = EINVAL;
        return -1;
    }
    time_ref->marker_abs_time_us = parsed.abs_time;
    time_ref->has_marker = 1;
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

    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t cursor = 0U;
    uint32_t record_index = 0U;
    uint64_t local_invalid_records = 0ULL;
    nvme_post_action_time_ref_t time_ref;
    memset(&time_ref, 0, sizeof(time_ref));
    uint32_t parse_len = data_len;
    uint32_t tail_bytes = data_len % NVME_POST_ACTION_RECORD_BYTES_SHORT;
    if (tail_bytes != 0U) {
        parse_len -= tail_bytes;
        ++local_invalid_records;
    }

    while (cursor < parse_len) {
        const unsigned char *record = bytes + cursor;
        uint64_t record_offset = offset_bytes + (uint64_t)cursor;
        uint64_t record_lo = load_le64_u(record);
        uint8_t op = (uint8_t)(record_lo & 0xFFU);
        int rc = 0;

        if ((data_len - cursor) >= NVME_POST_ACTION_RECORD_BYTES_LONG) {
            uint64_t record_hi_for_terminator = load_le64_u(record + 8U);
            uint8_t op_hi = (uint8_t)(record_hi_for_terminator & 0xFFU);
            if (op == 0x00U && op_hi == 0x00U) {
                return 0;
            }
        }

        if (op == NVME_POST_ACTION_OP_READ || op == NVME_POST_ACTION_OP_WRITE) {
            if ((parse_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
                ++local_invalid_records;
                break;
            }
            uint64_t record_hi = load_le64_u(record + 8U);
            rc = parse_rw_record(record_lo, record_hi, op, record_offset, record_index, &time_ref);
            cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
            ++record_index;
        } else if (op == NVME_POST_ACTION_OP_TRIM) {
            uint32_t consumed_bytes = 0U;
            uint32_t consumed_records = 0U;
            rc = parse_trim_group(bytes, parse_len, cursor, offset_bytes, record_index, &time_ref,
                                  &consumed_bytes, &consumed_records);
            if (rc != 0) {
                ++local_invalid_records;
                cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
                ++record_index;
                continue;
            }
            cursor += consumed_bytes;
            record_index += consumed_records;
        } else if (op == NVME_POST_ACTION_OP_STAT) {
#if NVME_POST_ACTION_SKIP_STAT
            rc = 0;
#else
            rc = parse_stat_record(record_lo, record_offset, record_index, &time_ref);
#endif
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
        } else if (op == NVME_POST_ACTION_OP_MARKER) {
            rc = parse_marker_record(record_lo, record_offset, record_index, &time_ref);
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
        } else {
            ++local_invalid_records;
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
            continue;
        }
        if (rc != 0) {
            ++local_invalid_records;
            continue;
        }
    }

    post_action_add_invalid_count(local_invalid_records);
    return 0;
}

int nvme_post_action_set_handler(nvme_read_post_action_t action, void *ctx) {
    if (action == NULL) {
        g_post_action = default_post_action;
        g_post_action_ctx = NULL;
        return 0;
    }
    g_post_action = action;
    g_post_action_ctx = ctx;
    return 0;
}

int nvme_post_action_process(void *data, uint32_t data_len, uint64_t offset_bytes) {
    if (g_post_action(g_post_action_ctx, data, data_len, offset_bytes) != 0) {
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

int nvme_post_action_set_debug(int enabled) {
    g_post_action_debug_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_debug(void) {
    return g_post_action_debug_enabled;
}

int nvme_post_action_set_sector_size(uint32_t sector_size) {
    return nvme_post_action_stats_init(sector_size);
}

int nvme_post_action_set_latency_enabled(int enabled) {
    nvme_post_action_latency_set_enabled(enabled);
    return 0;
}

int nvme_post_action_get_latency_enabled(void) {
    return nvme_post_action_latency_get_enabled();
}

void nvme_post_action_reset_latency_stats(void) {
    nvme_post_action_latency_reset();
}

void nvme_post_action_print_latency_report(void) {
    nvme_post_action_latency_print_summary();
}
