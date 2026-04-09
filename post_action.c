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
#define NVME_POST_ACTION_RATIO_BUCKETS 10U

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
    uint32_t time_rel;
    uint16_t log_write;
    uint16_t reserved;
    uint32_t hot_write;
    uint32_t folding_write;
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
static pthread_mutex_t g_post_action_marker_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_post_action_last_marker_abs_time_us = 0ULL;
static int g_post_action_has_last_marker = 0;
static uint64_t g_post_action_base_lba = 0ULL;
static uint32_t g_post_action_sector_size = (uint32_t)NVME_LBA_SIZE_BYTES;

static int default_post_action(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes);
static nvme_read_post_action_t g_post_action = default_post_action;
static void *g_post_action_ctx = NULL;
static int g_post_action_debug_enabled = 0;
static int g_post_action_lba_read_count_enabled = 0;
static int g_post_action_lba_w2fr_enabled = 0;
static int g_post_action_lba_life_cycle_enabled = 0;
static int g_post_action_qd_dist_enabled = 0;
static int g_post_action_wa_dist_enabled = 0;
static int g_post_action_read_size_dist_enabled = 0;
static int g_post_action_write_size_dist_enabled = 0;
static int g_post_action_trim_size_dist_enabled = 0;
static pthread_mutex_t g_post_action_workload_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_post_action_qd_hist[NVME_POST_ACTION_RATIO_BUCKETS];
static uint64_t g_post_action_wa_hist[NVME_POST_ACTION_RATIO_BUCKETS];
static uint64_t g_post_action_read_size_hist[NVME_POST_ACTION_RATIO_BUCKETS];
static uint64_t g_post_action_write_size_hist[NVME_POST_ACTION_RATIO_BUCKETS];
static uint64_t g_post_action_trim_size_hist[NVME_POST_ACTION_RATIO_BUCKETS];
static uint64_t g_post_action_qd_samples = 0ULL;
static uint64_t g_post_action_wa_samples = 0ULL;
static uint64_t g_post_action_read_size_samples = 0ULL;
static uint64_t g_post_action_write_size_samples = 0ULL;
static uint64_t g_post_action_trim_size_samples = 0ULL;

void nvme_post_action_reset_invalid_count(void) {
    pthread_mutex_lock(&g_post_action_invalid_mutex);
    g_post_action_invalid_records = 0ULL;
    pthread_mutex_unlock(&g_post_action_invalid_mutex);

    pthread_mutex_lock(&g_post_action_marker_mutex);
    g_post_action_last_marker_abs_time_us = 0ULL;
    g_post_action_has_last_marker = 0;
    pthread_mutex_unlock(&g_post_action_marker_mutex);
}

static void post_action_add_invalid_count(uint64_t delta) {
    if (delta == 0ULL) {
        return;
    }
    pthread_mutex_lock(&g_post_action_invalid_mutex);
    g_post_action_invalid_records += delta;
    pthread_mutex_unlock(&g_post_action_invalid_mutex);
}

static const char *label_qd(uint32_t idx) {
    static const char *labels[10] = {
        "0", "1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65-128", ">=129"
    };
    return labels[idx < 10U ? idx : 9U];
}

static const char *label_wa(uint32_t idx) {
    static const char *labels[10] = {
        "[1.0,1.4)", "[1.4,1.8)", "[1.8,2.2)", "[2.2,2.6)", "[2.6,3.0)",
        "[3.0,3.4)", "[3.4,3.8)", "[3.8,4.2)", "[4.2,4.6)", "[4.6,5.0]"
    };
    return labels[idx < 10U ? idx : 9U];
}

static const char *label_io_size(uint32_t idx) {
    static const char *labels[10] = {
        "0", "4K", "8K", "16K", "32K",
        "64K", "128K", "256K", "512K", ">=1M"
    };
    return labels[idx < 10U ? idx : 9U];
}

static uint32_t bucket_index_qd(uint16_t qd) {
    if (qd == 0U) {
        return 0U;
    }
    if (qd == 1U) {
        return 1U;
    }
    if (qd == 2U) {
        return 2U;
    }
    if (qd <= 4U) {
        return 3U;
    }
    if (qd <= 8U) {
        return 4U;
    }
    if (qd <= 16U) {
        return 5U;
    }
    if (qd <= 32U) {
        return 6U;
    }
    if (qd <= 64U) {
        return 7U;
    }
    if (qd <= 128U) {
        return 8U;
    }
    return 9U;
}

static uint32_t bucket_index_wa_x1000(uint32_t wa_x1000) {
    if (wa_x1000 <= 1000U) {
        return 0U;
    }
    if (wa_x1000 >= 5000U) {
        return 9U;
    }
    uint32_t idx = (wa_x1000 - 1000U) / 400U;
    if (idx >= NVME_POST_ACTION_RATIO_BUCKETS) {
        idx = NVME_POST_ACTION_RATIO_BUCKETS - 1U;
    }
    return idx;
}

static uint32_t bucket_index_io_size_4k(uint64_t len_lba) {
    if (len_lba == 0ULL) {
        return 0U;
    }
    if (len_lba == 1ULL) {
        return 1U;
    }
    if (len_lba <= 2ULL) {
        return 2U;
    }
    if (len_lba <= 4ULL) {
        return 3U;
    }
    if (len_lba <= 8ULL) {
        return 4U;
    }
    if (len_lba <= 16ULL) {
        return 5U;
    }
    if (len_lba <= 32ULL) {
        return 6U;
    }
    if (len_lba <= 64ULL) {
        return 7U;
    }
    if (len_lba <= 128ULL) {
        return 8U;
    }
    return 9U;
}

static void record_qd_and_wa(uint16_t qd, uint32_t hot_write_4k, uint32_t folding_write_4k) {
    if (g_post_action_qd_dist_enabled == 0 && g_post_action_wa_dist_enabled == 0) {
        return;
    }
    pthread_mutex_lock(&g_post_action_workload_mutex);
    if (g_post_action_qd_dist_enabled != 0) {
        uint32_t idx = bucket_index_qd(qd);
        ++g_post_action_qd_hist[idx];
        ++g_post_action_qd_samples;
    }
    if (g_post_action_wa_dist_enabled != 0 && hot_write_4k != 0U) {
        // WA uses (hot_write + folding_write) / hot_write.
        uint64_t numer = (uint64_t)hot_write_4k + (uint64_t)folding_write_4k;
        uint32_t wa_x1000 = (uint32_t)((numer * 1000ULL) / (uint64_t)hot_write_4k);
        uint32_t idx = bucket_index_wa_x1000(wa_x1000);
        ++g_post_action_wa_hist[idx];
        ++g_post_action_wa_samples;
    }
    pthread_mutex_unlock(&g_post_action_workload_mutex);
}

static void record_io_size(uint8_t op, uint64_t len_lba) {
    if (op == NVME_POST_ACTION_OP_READ && g_post_action_read_size_dist_enabled == 0) {
        return;
    }
    if (op == NVME_POST_ACTION_OP_WRITE && g_post_action_write_size_dist_enabled == 0) {
        return;
    }
    if (op == NVME_POST_ACTION_OP_TRIM && g_post_action_trim_size_dist_enabled == 0) {
        return;
    }
    uint32_t idx = bucket_index_io_size_4k(len_lba);
    pthread_mutex_lock(&g_post_action_workload_mutex);
    if (op == NVME_POST_ACTION_OP_READ && g_post_action_read_size_dist_enabled != 0) {
        ++g_post_action_read_size_hist[idx];
        ++g_post_action_read_size_samples;
    } else if (op == NVME_POST_ACTION_OP_WRITE && g_post_action_write_size_dist_enabled != 0) {
        ++g_post_action_write_size_hist[idx];
        ++g_post_action_write_size_samples;
    } else if (op == NVME_POST_ACTION_OP_TRIM && g_post_action_trim_size_dist_enabled != 0) {
        ++g_post_action_trim_size_hist[idx];
        ++g_post_action_trim_size_samples;
    }
    pthread_mutex_unlock(&g_post_action_workload_mutex);
}

static void print_histogram_section(const char *title,
                                    const char *item_name,
                                    const uint64_t hist[10],
                                    uint64_t total_samples,
                                    const char *(*label_fn)(uint32_t)) {
    if (hist == NULL || label_fn == NULL) {
        return;
    }
    fprintf(stderr, "%s:\n", title);
    for (uint32_t i = 0U; i < 10U; ++i) {
        double pct = 0.0;
        if (total_samples != 0ULL) {
            pct = ((double)hist[i] * 100.0) / (double)total_samples;
        }
        fprintf(stderr, "  %-16s %12s count=%llu (%.2f%%)\n",
                item_name,
                label_fn(i),
                (unsigned long long)hist[i],
                pct);
    }
}

static uint64_t marker_record_to_lba_locked(uint64_t record_offset_bytes) {
    uint32_t sector_size = g_post_action_sector_size == 0U ?
        (uint32_t)NVME_LBA_SIZE_BYTES : g_post_action_sector_size;
    return g_post_action_base_lba + (record_offset_bytes / (uint64_t)sector_size);
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

#if NVME_POST_ACTION_DEBUG
static void debug_print_record_hex(const unsigned char *record,
                                   uint32_t record_bytes,
                                   uint64_t record_offset,
                                   uint32_t record_index,
                                   uint8_t op) {
    if (record == NULL || record_bytes == 0U) {
        return;
    }
    fprintf(stderr,
            "post action record hex: offset=%llu record=%u op=0x%02x bytes=%u data=",
            (unsigned long long)record_offset,
            (unsigned int)record_index,
            (unsigned int)op,
            (unsigned int)record_bytes);
    for (uint32_t i = 0U; i < record_bytes; ++i) {
        fprintf(stderr, "%02x", (unsigned int)record[i]);
        if ((i + 1U) < record_bytes) {
            fputc(' ', stderr);
        }
    }
    fputc('\n', stderr);
}
#endif

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
        if (g_post_action_debug_enabled != 0) {
            fprintf(stderr,
                    "post action invalid rw reserved: offset=%llu record=%u op=0x%02x reserved=0x%04x\n",
                    (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)op,
                    (unsigned int)reserved);
        }
        return -1;
    }
    if (resolve_abs_time_us(parsed.time_rel, time_ref, &abs_time_us) != 0) {
        if (g_post_action_debug_enabled != 0) {
            fprintf(stderr,
                    "post action missing marker for rw time: offset=%llu record=%u op=0x%02x time_rel=%u\n",
                    (unsigned long long)offset_bytes,
                    (unsigned int)record_index,
                    (unsigned int)op,
                    (unsigned int)parsed.time_rel);
        }
        return -1;
    }

    if (op == NVME_POST_ACTION_OP_WRITE) {
        nvme_post_action_stats_update_write(parsed.start_lba, (uint64_t)parsed.length, abs_time_us);
        nvme_post_action_latency_record_write(parsed.latency);
        record_io_size(op, (uint64_t)parsed.length);
    } else if (op == NVME_POST_ACTION_OP_READ) {
        nvme_post_action_stats_update_read(parsed.start_lba, (uint64_t)parsed.length, abs_time_us);
        nvme_post_action_latency_record_read(parsed.latency);
        record_io_size(op, (uint64_t)parsed.length);
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
        if (g_post_action_debug_enabled != 0) {
            fprintf(stderr,
                    "post action invalid trim reserved: offset=%llu record=%u reserved=0x%02x\n",
                    (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)reserved);
        }
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
        if (g_post_action_debug_enabled != 0) {
            fprintf(stderr,
                    "post action missing marker for trim time: offset=%llu record=%u\n",
                    (unsigned long long)(base_offset_bytes + (uint64_t)cursor),
                    (unsigned int)record_index);
        }
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
        nvme_post_action_latency_record_trim(fields.time_rel);
        record_io_size(NVME_POST_ACTION_OP_TRIM, (uint64_t)fields.length);
    }
    parsed.range_count = parsed.total_ranges;
    (void)parsed;
    *consumed_bytes = (uint32_t)parsed.total_ranges * NVME_POST_ACTION_RECORD_BYTES_LONG;
    *consumed_records = (uint32_t)parsed.total_ranges;
    return 0;
}

#if !NVME_POST_ACTION_SKIP_STAT
static int parse_stat_record(uint64_t record_lo,
                             uint64_t record_hi,
                             uint64_t offset_bytes,
                             uint32_t record_index,
                             nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_stat_t parsed;
    parsed.meta.op = NVME_POST_ACTION_OP_STAT;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    parsed.qd = (uint16_t)((record_lo >> 8U) & 0xFFFFU);
    parsed.time_rel = (uint32_t)((record_lo >> 24U) & NVME_POST_ACTION_U24_MASK);
    parsed.log_write = (uint16_t)((record_lo >> 48U) & 0xFFFFU);
    parsed.reserved = (uint16_t)(record_hi & 0xFFFFU);
    parsed.hot_write = (uint32_t)((record_hi >> 16U) & NVME_POST_ACTION_U24_MASK);
    parsed.folding_write = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);
    uint64_t abs_time_us = 0ULL;

    if (parsed.reserved != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_abs_time_us(parsed.time_rel, time_ref, &abs_time_us) != 0) {
        return -1;
    }
    if (nvme_post_action_stats_record_stat(abs_time_us,
                                           parsed.qd,
                                           parsed.hot_write,
                                           parsed.folding_write) != 0) {
        return -1;
    }
    record_qd_and_wa(parsed.qd, parsed.hot_write, parsed.folding_write);
#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action stat: offset=%llu record=%u qd=%u time_rel=%u abs_time_us=%llu "
            "log_write_4k=%u hot_write_4k=%u folding_write_4k=%u\n",
            (unsigned long long)parsed.meta.offset_bytes,
            (unsigned int)parsed.meta.record_index,
            (unsigned int)parsed.qd,
            (unsigned int)parsed.time_rel,
            (unsigned long long)abs_time_us,
            (unsigned int)parsed.log_write,
            (unsigned int)parsed.hot_write,
            (unsigned int)parsed.folding_write);
#else
    (void)parsed;
#endif
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

    pthread_mutex_lock(&g_post_action_marker_mutex);
    if (g_post_action_has_last_marker != 0 &&
        parsed.abs_time <= g_post_action_last_marker_abs_time_us) {
        uint64_t overwrite_lba = marker_record_to_lba_locked(offset_bytes);
        uint64_t prev_abs_time = g_post_action_last_marker_abs_time_us;
        pthread_mutex_unlock(&g_post_action_marker_mutex);
        if (g_post_action_debug_enabled != 0) {
            fprintf(stderr,
                    "post action warning: overwrite detected, marker timestamp not increasing: "
                    "prev_abs_time_us=%llu curr_abs_time_us=%llu lba=%llu offset=%llu record=%u\n",
                    (unsigned long long)prev_abs_time,
                    (unsigned long long)parsed.abs_time,
                    (unsigned long long)overwrite_lba,
                    (unsigned long long)offset_bytes,
                    (unsigned int)record_index);
        }
        errno = ECANCELED;
        return -1;
    }
    g_post_action_last_marker_abs_time_us = parsed.abs_time;
    g_post_action_has_last_marker = 1;
    pthread_mutex_unlock(&g_post_action_marker_mutex);

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
    // Keep time baseline continuous across callback invocations/chunks.
    pthread_mutex_lock(&g_post_action_marker_mutex);
    time_ref.marker_abs_time_us = g_post_action_last_marker_abs_time_us;
    time_ref.has_marker = g_post_action_has_last_marker;
    pthread_mutex_unlock(&g_post_action_marker_mutex);
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

        if (time_ref.has_marker == 0 && op != NVME_POST_ACTION_OP_MARKER) {
            uint32_t skip_bytes = NVME_POST_ACTION_RECORD_BYTES_SHORT;
            if (op == NVME_POST_ACTION_OP_READ ||
                op == NVME_POST_ACTION_OP_WRITE ||
                op == NVME_POST_ACTION_OP_TRIM ||
                op == NVME_POST_ACTION_OP_STAT) {
                skip_bytes = NVME_POST_ACTION_RECORD_BYTES_LONG;
            }
            if ((parse_len - cursor) < skip_bytes) {
                ++local_invalid_records;
                break;
            }
            // Before first marker, all non-marker records are discarded as invalid.
            ++local_invalid_records;
            cursor += skip_bytes;
            ++record_index;
            continue;
        }

        if (op == 0x00U) {
#if NVME_POST_ACTION_DEBUG
            debug_print_record_hex(record, NVME_POST_ACTION_RECORD_BYTES_SHORT,
                                   record_offset, record_index, op);
#endif
            // A full-zero 8-byte record is treated as end-of-valid-log marker.
            if (record_lo == 0ULL) {
                errno = ENODATA;
                post_action_add_invalid_count(local_invalid_records);
                return -1;
            }
            // Non-zero payload with op==0 is treated as invalid/noise and skipped.
            ++local_invalid_records;
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
            continue;
        }

        if (op == NVME_POST_ACTION_OP_READ || op == NVME_POST_ACTION_OP_WRITE) {
            if ((parse_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
                ++local_invalid_records;
                break;
            }
#if NVME_POST_ACTION_DEBUG
            debug_print_record_hex(record, NVME_POST_ACTION_RECORD_BYTES_LONG,
                                   record_offset, record_index, op);
#endif
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
                if (errno == ECANCELED) {
                    post_action_add_invalid_count(local_invalid_records);
                    return -1;
                }
                ++local_invalid_records;
                cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
                ++record_index;
                continue;
            }
#if NVME_POST_ACTION_DEBUG
            for (uint32_t i = 0U; i < consumed_records; ++i) {
                uint32_t rec_cursor = cursor + (i * NVME_POST_ACTION_RECORD_BYTES_LONG);
                const unsigned char *trim_rec = bytes + rec_cursor;
                uint64_t trim_offset = offset_bytes + (uint64_t)rec_cursor;
                uint64_t trim_lo = load_le64_u(trim_rec);
                uint8_t trim_op = (uint8_t)(trim_lo & 0xFFU);
                debug_print_record_hex(trim_rec, NVME_POST_ACTION_RECORD_BYTES_LONG,
                                       trim_offset, record_index + i, trim_op);
            }
#endif
            cursor += consumed_bytes;
            record_index += consumed_records;
        } else if (op == NVME_POST_ACTION_OP_STAT) {
            if ((parse_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
                ++local_invalid_records;
                break;
            }
#if NVME_POST_ACTION_DEBUG
            debug_print_record_hex(record, NVME_POST_ACTION_RECORD_BYTES_LONG,
                                   record_offset, record_index, op);
#endif
            uint64_t record_hi = load_le64_u(record + 8U);
#if NVME_POST_ACTION_SKIP_STAT
            rc = 0;
#else
            rc = parse_stat_record(record_lo, record_hi, record_offset, record_index, &time_ref);
#endif
            cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
            ++record_index;
        } else if (op == NVME_POST_ACTION_OP_MARKER) {
#if NVME_POST_ACTION_DEBUG
            debug_print_record_hex(record, NVME_POST_ACTION_RECORD_BYTES_SHORT,
                                   record_offset, record_index, op);
#endif
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
            if (errno == ECANCELED) {
                post_action_add_invalid_count(local_invalid_records);
                return -1;
            }
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
    nvme_post_action_stats_print_summary_debug(g_post_action_debug_enabled);
    nvme_post_action_latency_set_debug(g_post_action_debug_enabled);
    return 0;
}

int nvme_post_action_get_debug(void) {
    return g_post_action_debug_enabled;
}

int nvme_post_action_set_sector_size(uint32_t sector_size) {
    if (sector_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_post_action_marker_mutex);
    g_post_action_sector_size = sector_size;
    pthread_mutex_unlock(&g_post_action_marker_mutex);
    return nvme_post_action_stats_init(sector_size);
}

int nvme_post_action_set_base_lba(uint64_t base_lba) {
    pthread_mutex_lock(&g_post_action_marker_mutex);
    g_post_action_base_lba = base_lba;
    g_post_action_last_marker_abs_time_us = 0ULL;
    g_post_action_has_last_marker = 0;
    pthread_mutex_unlock(&g_post_action_marker_mutex);
    return 0;
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
    if (g_post_action_debug_enabled == 0) {
        return;
    }
    nvme_post_action_latency_print_summary(g_post_action_debug_enabled);
}

int nvme_post_action_set_lba_read_count_enabled(int enabled) {
    g_post_action_lba_read_count_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_set_lba_w2fr_enabled(int enabled) {
    g_post_action_lba_w2fr_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_set_lba_life_cycle_enabled(int enabled) {
    g_post_action_lba_life_cycle_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_lba_read_count_enabled(void) {
    return g_post_action_lba_read_count_enabled;
}

int nvme_post_action_get_lba_w2fr_enabled(void) {
    return g_post_action_lba_w2fr_enabled;
}

int nvme_post_action_get_lba_life_cycle_enabled(void) {
    return g_post_action_lba_life_cycle_enabled;
}

int nvme_post_action_set_qd_dist_enabled(int enabled) {
    g_post_action_qd_dist_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_qd_dist_enabled(void) {
    return g_post_action_qd_dist_enabled;
}

int nvme_post_action_set_wa_dist_enabled(int enabled) {
    g_post_action_wa_dist_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_wa_dist_enabled(void) {
    return g_post_action_wa_dist_enabled;
}

int nvme_post_action_set_read_size_dist_enabled(int enabled) {
    g_post_action_read_size_dist_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_read_size_dist_enabled(void) {
    return g_post_action_read_size_dist_enabled;
}

int nvme_post_action_set_write_size_dist_enabled(int enabled) {
    g_post_action_write_size_dist_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_write_size_dist_enabled(void) {
    return g_post_action_write_size_dist_enabled;
}

int nvme_post_action_set_trim_size_dist_enabled(int enabled) {
    g_post_action_trim_size_dist_enabled = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_get_trim_size_dist_enabled(void) {
    return g_post_action_trim_size_dist_enabled;
}

void nvme_post_action_print_lba_stats_report(void) {
    if (g_post_action_debug_enabled == 0) {
        return;
    }
    if (g_post_action_lba_read_count_enabled == 0 &&
        g_post_action_lba_w2fr_enabled == 0 &&
        g_post_action_lba_life_cycle_enabled == 0) {
        return;
    }
    nvme_post_action_stats_print_ratio_summary(g_post_action_lba_read_count_enabled,
                                               g_post_action_lba_w2fr_enabled,
                                               g_post_action_lba_life_cycle_enabled);
}

void nvme_post_action_print_workload_stats_report(void) {
    if (g_post_action_debug_enabled == 0) {
        return;
    }
    if (g_post_action_qd_dist_enabled == 0 &&
        g_post_action_wa_dist_enabled == 0 &&
        g_post_action_read_size_dist_enabled == 0 &&
        g_post_action_write_size_dist_enabled == 0 &&
        g_post_action_trim_size_dist_enabled == 0) {
        return;
    }
    pthread_mutex_lock(&g_post_action_workload_mutex);
    if (g_post_action_qd_dist_enabled != 0) {
        print_histogram_section("QD distribution", "QD", g_post_action_qd_hist,
                                g_post_action_qd_samples, label_qd);
    }
    if (g_post_action_wa_dist_enabled != 0) {
        print_histogram_section("WA distribution", "WA", g_post_action_wa_hist,
                                g_post_action_wa_samples, label_wa);
    }
    if (g_post_action_read_size_dist_enabled != 0) {
        print_histogram_section("Read Size distribution (4K blocks)", "Read Size",
                                g_post_action_read_size_hist, g_post_action_read_size_samples,
                                label_io_size);
    }
    if (g_post_action_write_size_dist_enabled != 0) {
        print_histogram_section("Write Size distribution (4K blocks)", "Write Size",
                                g_post_action_write_size_hist, g_post_action_write_size_samples,
                                label_io_size);
    }
    if (g_post_action_trim_size_dist_enabled != 0) {
        print_histogram_section("Trim Size distribution (4K blocks)", "Trim Size",
                                g_post_action_trim_size_hist, g_post_action_trim_size_samples,
                                label_io_size);
    }
    pthread_mutex_unlock(&g_post_action_workload_mutex);
}
