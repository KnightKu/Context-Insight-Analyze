#define _POSIX_C_SOURCE 200809L
#include "post_action_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NVME_POST_ACTION_RATIO_BUCKETS 10U

typedef struct {
    uint64_t key;
    uint64_t last_write_abs_us;
    uint8_t first_read_seen;
    uint8_t used;
} lba_active_write_entry_t;

static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_stats_inited = 0;
static int g_stats_enabled = 0;
static uint32_t g_sector_size = NVME_POST_ACTION_DEFAULT_SECTOR_SIZE;
static uint64_t g_bucket_count = 0ULL;
static uint64_t g_total_bytes = 0ULL;
static nvme_post_action_lba_stat_t *g_stats = NULL;
static uint32_t g_latency_threshold_ms[NVME_POST_ACTION_LATENCY_CODE_MAX + 1U];
static lba_active_write_entry_t *g_active_writes = NULL;
static uint64_t g_active_writes_cap = 0ULL;
static uint64_t g_active_writes_count = 0ULL;
static uint32_t g_advanced_max_scale = 0U;
static uint64_t *g_advanced_hist = NULL;
static uint64_t g_advanced_hist_size = 0ULL;
static uint64_t g_mdts_bytes = 0ULL;
static nvme_post_action_stat_sample_t *g_stat_samples = NULL;
static uint64_t g_stat_samples_count = 0ULL;
static uint64_t g_stat_samples_cap = 0ULL;

typedef struct {
    uint64_t read_count_hist[NVME_POST_ACTION_RATIO_BUCKETS];
    uint64_t w2fr_ms_hist[NVME_POST_ACTION_RATIO_BUCKETS];
    uint64_t life_cycle_ms_hist[NVME_POST_ACTION_RATIO_BUCKETS];
    uint64_t read_count_non_zero_buckets;
    uint64_t w2fr_non_zero_buckets;
    uint64_t life_cycle_non_zero_buckets;
} nvme_post_action_lba_ratio_summary_t;

static inline uint64_t advanced_hist_index(uint32_t scale_idx, uint16_t life_code) {
    return ((uint64_t)scale_idx * ((uint64_t)NVME_POST_ACTION_LATENCY_CODE_MAX + 1ULL)) +
           (uint64_t)life_code;
}

static uint64_t floor_pow2_u64(uint64_t x) {
    if (x == 0ULL) {
        return 0ULL;
    }
    uint64_t p = 1ULL;
    while ((p << 1U) <= x) {
        p <<= 1U;
    }
    return p;
}

static void build_latency_threshold_table(void) {
    uint64_t acc = 0ULL;
    g_latency_threshold_ms[0] = 0U;
    for (uint32_t i = 1U; i <= NVME_POST_ACTION_LATENCY_CODE_MAX; ++i) {
        uint32_t step = 1U;
        if (i >= 32768U) {
            step = 256U;
        } else if (i >= 8192U) {
            step = 16U;
        } else if (i >= 1024U) {
            step = 2U;
        }
        acc += (uint64_t)step;
        if (acc > (uint64_t)UINT32_MAX) {
            acc = (uint64_t)UINT32_MAX;
        }
        g_latency_threshold_ms[i] = (uint32_t)acc;
    }
}

static uint16_t encode_duration_ms_non_linear(uint32_t duration_ms) {
    if (duration_ms == 0U) {
        return 0U;
    }
    if (duration_ms >= g_latency_threshold_ms[NVME_POST_ACTION_LATENCY_CODE_MAX]) {
        return (uint16_t)NVME_POST_ACTION_LATENCY_CODE_MAX;
    }
    uint32_t lo = 0U;
    uint32_t hi = NVME_POST_ACTION_LATENCY_CODE_MAX;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2U);
        if (g_latency_threshold_ms[mid] < duration_ms) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return (uint16_t)lo;
}

static uint32_t decode_duration_ms_non_linear(uint16_t code) {
    return g_latency_threshold_ms[(uint32_t)code];
}

static uint32_t ratio_bucket_index_u8(uint8_t value) {
    if (value == 0U) {
        return 0U;
    }
    uint32_t bucket = ((uint32_t)value + 24U) / 25U;
    if (bucket >= NVME_POST_ACTION_RATIO_BUCKETS) {
        bucket = NVME_POST_ACTION_RATIO_BUCKETS - 1U;
    }
    return bucket;
}

static uint32_t ratio_bucket_index_ms(uint32_t ms) {
    if (ms == 0U) {
        return 0U;
    }
    if (ms < 1000U) {
        return 1U;
    }
    if (ms < 5000U) {
        return 2U;
    }
    if (ms < 10000U) {
        return 3U;
    }
    if (ms < 30000U) {
        return 4U;
    }
    if (ms < 60000U) {
        return 5U;
    }
    if (ms < 300000U) {
        return 6U;
    }
    if (ms < 1800000U) {
        return 7U;
    }
    if (ms < 3600000U) {
        return 8U;
    }
    return 9U;
}

static void ratio_label_read_count(uint32_t idx, char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0U) {
        return;
    }
    switch (idx) {
        case 0U:
            (void)snprintf(buf, buf_len, "0");
            break;
        case 1U:
            (void)snprintf(buf, buf_len, "1-25");
            break;
        case 2U:
            (void)snprintf(buf, buf_len, "26-50");
            break;
        case 3U:
            (void)snprintf(buf, buf_len, "51-75");
            break;
        case 4U:
            (void)snprintf(buf, buf_len, "76-100");
            break;
        case 5U:
            (void)snprintf(buf, buf_len, "101-125");
            break;
        case 6U:
            (void)snprintf(buf, buf_len, "126-150");
            break;
        case 7U:
            (void)snprintf(buf, buf_len, "151-175");
            break;
        case 8U:
            (void)snprintf(buf, buf_len, "176-200");
            break;
        default:
            (void)snprintf(buf, buf_len, "201-255");
            break;
    }
}

static void ratio_label_latency_ms(uint32_t idx, char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0U) {
        return;
    }
    switch (idx) {
        case 0U:
            (void)snprintf(buf, buf_len, "0ms");
            break;
        case 1U:
            (void)snprintf(buf, buf_len, "(0ms,1s)");
            break;
        case 2U:
            (void)snprintf(buf, buf_len, "[1s,5s)");
            break;
        case 3U:
            (void)snprintf(buf, buf_len, "[5s,10s)");
            break;
        case 4U:
            (void)snprintf(buf, buf_len, "[10s,30s)");
            break;
        case 5U:
            (void)snprintf(buf, buf_len, "[30s,1m)");
            break;
        case 6U:
            (void)snprintf(buf, buf_len, "[1m,5m)");
            break;
        case 7U:
            (void)snprintf(buf, buf_len, "[5m,30m)");
            break;
        case 8U:
            (void)snprintf(buf, buf_len, "[30m,1h)");
            break;
        default:
            (void)snprintf(buf, buf_len, ">=1h");
            break;
    }
}

static int build_lba_ratio_summary_locked(nvme_post_action_lba_ratio_summary_t *out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (g_stats_enabled == 0 || g_stats == NULL || g_bucket_count == 0ULL) {
        return 0;
    }
    for (uint64_t i = 0ULL; i < g_bucket_count; ++i) {
        const nvme_post_action_lba_stat_t *s = &g_stats[i];
        uint32_t rc_idx = ratio_bucket_index_u8(s->read_count);
        ++out->read_count_hist[rc_idx];
        if (s->read_count != 0U) {
            ++out->read_count_non_zero_buckets;
        }

        uint32_t w2fr_ms = decode_duration_ms_non_linear(s->write_to_first_read_latency);
        uint32_t w2fr_idx = ratio_bucket_index_ms(w2fr_ms);
        ++out->w2fr_ms_hist[w2fr_idx];
        if (s->write_to_first_read_latency != 0U) {
            ++out->w2fr_non_zero_buckets;
        }

        uint32_t life_ms = decode_duration_ms_non_linear(s->life_cycle_latency);
        uint32_t life_idx = ratio_bucket_index_ms(life_ms);
        ++out->life_cycle_ms_hist[life_idx];
        if (s->life_cycle_latency != 0U) {
            ++out->life_cycle_non_zero_buckets;
        }
    }
    return 0;
}

static void print_ratio_hist_line(const char *name,
                                  uint64_t hit,
                                  uint64_t total,
                                  const char *label) {
    double pct = 0.0;
    if (total != 0ULL) {
        pct = ((double)hit * 100.0) / (double)total;
    }
    fprintf(stderr, "  %-28s %12s count=%" PRIu64 " (%.2f%%)\n",
            name, label, hit, pct);
}

static int should_print_section(int print_read_count,
                                int print_w2fr,
                                int print_life_cycle) {
    return (print_read_count != 0 || print_w2fr != 0 || print_life_cycle != 0) ? 1 : 0;
}

void nvme_post_action_stats_print_ratio_summary(int print_read_count,
                                                int print_w2fr,
                                                int print_life_cycle) {
    if (should_print_section(print_read_count, print_w2fr, print_life_cycle) == 0) {
        return;
    }
    nvme_post_action_lba_ratio_summary_t summary;
    pthread_mutex_lock(&g_stats_mutex);
    int rc = build_lba_ratio_summary_locked(&summary);
    uint64_t total_buckets = g_bucket_count;
    pthread_mutex_unlock(&g_stats_mutex);
    if (rc != 0) {
        return;
    }
    uint64_t w2fr_print_hist[NVME_POST_ACTION_RATIO_BUCKETS];
    uint64_t life_print_hist[NVME_POST_ACTION_RATIO_BUCKETS];
    memcpy(w2fr_print_hist, summary.w2fr_ms_hist, sizeof(w2fr_print_hist));
    memcpy(life_print_hist, summary.life_cycle_ms_hist, sizeof(life_print_hist));
    // Display-only adjustment: merge 0ms counts into >=1h bucket.
    w2fr_print_hist[NVME_POST_ACTION_RATIO_BUCKETS - 1U] += w2fr_print_hist[0U];
    w2fr_print_hist[0U] = 0ULL;
    life_print_hist[NVME_POST_ACTION_RATIO_BUCKETS - 1U] += life_print_hist[0U];
    life_print_hist[0U] = 0ULL;

    fprintf(stderr, "lba ratio summary (bucket=%llu bytes):\n",
            (unsigned long long)NVME_POST_ACTION_STATS_BLOCK_BYTES);
    if (print_read_count != 0) {
        fprintf(stderr, "Read Count distribution:\n");
        for (uint32_t i = 0U; i < NVME_POST_ACTION_RATIO_BUCKETS; ++i) {
            char label[32];
            ratio_label_read_count(i, label, sizeof(label));
            print_ratio_hist_line("Read Count", summary.read_count_hist[i], total_buckets, label);
        }
        fprintf(stderr, "  non-zero buckets=%" PRIu64 " / %" PRIu64 " (%.2f%%)\n",
                summary.read_count_non_zero_buckets,
                total_buckets,
                total_buckets == 0ULL ? 0.0 :
                ((double)summary.read_count_non_zero_buckets * 100.0) / (double)total_buckets);
    }

    if (print_w2fr != 0) {
        fprintf(stderr, "Write-to-First-Read Latency(real ms) distribution:\n");
        for (uint32_t i = 0U; i < NVME_POST_ACTION_RATIO_BUCKETS; ++i) {
            char label[32];
            ratio_label_latency_ms(i, label, sizeof(label));
            print_ratio_hist_line("Write-to-First-Read", w2fr_print_hist[i], total_buckets, label);
        }
        fprintf(stderr, "  non-zero buckets=%" PRIu64 " / %" PRIu64 " (%.2f%%)\n",
                summary.w2fr_non_zero_buckets,
                total_buckets,
                total_buckets == 0ULL ? 0.0 :
                ((double)summary.w2fr_non_zero_buckets * 100.0) / (double)total_buckets);
    }

    if (print_life_cycle != 0) {
        fprintf(stderr, "LBA Life Cycle(real ms) distribution:\n");
        for (uint32_t i = 0U; i < NVME_POST_ACTION_RATIO_BUCKETS; ++i) {
            char label[32];
            ratio_label_latency_ms(i, label, sizeof(label));
            print_ratio_hist_line("LBA Life Cycle", life_print_hist[i], total_buckets, label);
        }
        fprintf(stderr, "  non-zero buckets=%" PRIu64 " / %" PRIu64 " (%.2f%%)\n",
                summary.life_cycle_non_zero_buckets,
                total_buckets,
                total_buckets == 0ULL ? 0.0 :
                ((double)summary.life_cycle_non_zero_buckets * 100.0) / (double)total_buckets);
    }
}

static uint64_t hash_u64(uint64_t x) {
    x ^= x >> 33U;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33U;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33U;
    return x;
}

static int active_writes_rehash(uint64_t new_cap) {
    lba_active_write_entry_t *new_map =
        (lba_active_write_entry_t *)calloc((size_t)new_cap, sizeof(lba_active_write_entry_t));
    if (new_map == NULL) {
        return -1;
    }
    for (uint64_t i = 0ULL; i < g_active_writes_cap; ++i) {
        if (g_active_writes[i].used == 0U) {
            continue;
        }
        uint64_t key = g_active_writes[i].key;
        uint64_t pos = hash_u64(key) & (new_cap - 1ULL);
        while (new_map[pos].used != 0U) {
            pos = (pos + 1ULL) & (new_cap - 1ULL);
        }
        new_map[pos] = g_active_writes[i];
    }
    free(g_active_writes);
    g_active_writes = new_map;
    g_active_writes_cap = new_cap;
    return 0;
}

static int active_writes_init(uint64_t initial_cap) {
    g_active_writes = (lba_active_write_entry_t *)calloc((size_t)initial_cap,
                                                         sizeof(lba_active_write_entry_t));
    if (g_active_writes == NULL) {
        return -1;
    }
    g_active_writes_cap = initial_cap;
    g_active_writes_count = 0ULL;
    return 0;
}

static lba_active_write_entry_t *active_writes_lookup(uint64_t key) {
    if (g_active_writes == NULL || g_active_writes_cap == 0ULL) {
        return NULL;
    }
    uint64_t pos = hash_u64(key) & (g_active_writes_cap - 1ULL);
    for (uint64_t probe = 0ULL; probe < g_active_writes_cap; ++probe) {
        lba_active_write_entry_t *entry = &g_active_writes[pos];
        if (entry->used == 0U) {
            return NULL;
        }
        if (entry->key == key) {
            return entry;
        }
        pos = (pos + 1ULL) & (g_active_writes_cap - 1ULL);
    }
    return NULL;
}

static lba_active_write_entry_t *active_writes_get_or_insert(uint64_t key, int *is_new) {
    if (is_new != NULL) {
        *is_new = 0;
    }
    if (g_active_writes == NULL || g_active_writes_cap == 0ULL) {
        return NULL;
    }
    if ((g_active_writes_count * 10ULL) >= (g_active_writes_cap * 7ULL)) {
        if (active_writes_rehash(g_active_writes_cap << 1U) != 0) {
            return NULL;
        }
    }
    uint64_t pos = hash_u64(key) & (g_active_writes_cap - 1ULL);
    for (uint64_t probe = 0ULL; probe < g_active_writes_cap; ++probe) {
        lba_active_write_entry_t *entry = &g_active_writes[pos];
        if (entry->used == 0U) {
            entry->used = 1U;
            entry->key = key;
            entry->last_write_abs_us = 0ULL;
            entry->first_read_seen = 0U;
            ++g_active_writes_count;
            if (is_new != NULL) {
                *is_new = 1;
            }
            return entry;
        }
        if (entry->key == key) {
            return entry;
        }
        pos = (pos + 1ULL) & (g_active_writes_cap - 1ULL);
    }
    return NULL;
}

static int stats_index_for_lba_range_locked(uint64_t start_lba,
                                            uint64_t len_lba,
                                            uint64_t *idx_begin,
                                            uint64_t *idx_end_exclusive) {
    if (idx_begin == NULL || idx_end_exclusive == NULL) {
        return -1;
    }
    if (len_lba == 0ULL) {
        *idx_begin = 0ULL;
        *idx_end_exclusive = 0ULL;
        return 0;
    }
    uint64_t sector_bytes = (uint64_t)g_sector_size;
    uint64_t start_bytes = start_lba * sector_bytes;
    uint64_t end_bytes = start_bytes + (len_lba * sector_bytes);
    uint64_t begin = start_bytes / NVME_POST_ACTION_STATS_BLOCK_BYTES;
    uint64_t end_exclusive =
        (end_bytes + NVME_POST_ACTION_STATS_BLOCK_BYTES - 1ULL) / NVME_POST_ACTION_STATS_BLOCK_BYTES;
    if (begin >= g_bucket_count) {
        *idx_begin = g_bucket_count;
        *idx_end_exclusive = g_bucket_count;
        return 0;
    }
    if (end_exclusive > g_bucket_count) {
        end_exclusive = g_bucket_count;
    }
    *idx_begin = begin;
    *idx_end_exclusive = end_exclusive;
    return 0;
}

static uint32_t duration_us_to_ms(uint64_t duration_us) {
    uint64_t ms = duration_us / 1000ULL;
    if (ms > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)ms;
}

static int stat_samples_ensure_capacity_locked(uint64_t want_count) {
    if (want_count <= g_stat_samples_cap) {
        return 0;
    }
    uint64_t new_cap = (g_stat_samples_cap == 0ULL) ? 1024ULL : g_stat_samples_cap;
    while (new_cap < want_count) {
        if (new_cap > (UINT64_MAX / 2ULL)) {
            errno = ENOMEM;
            return -1;
        }
        new_cap <<= 1U;
    }
    if (new_cap > ((uint64_t)SIZE_MAX / sizeof(nvme_post_action_stat_sample_t))) {
        errno = ENOMEM;
        return -1;
    }
    nvme_post_action_stat_sample_t *new_buf =
        (nvme_post_action_stat_sample_t *)realloc(g_stat_samples,
                                                  (size_t)new_cap * sizeof(nvme_post_action_stat_sample_t));
    if (new_buf == NULL) {
        errno = ENOMEM;
        return -1;
    }
    g_stat_samples = new_buf;
    g_stat_samples_cap = new_cap;
    return 0;
}

static void update_advanced_life_cycle_hist_locked(uint64_t begin_idx,
                                                   uint64_t end_exclusive,
                                                   uint16_t life_code) {
    if (g_advanced_hist == NULL || begin_idx >= end_exclusive) {
        return;
    }
    uint64_t cur = begin_idx;
    while (cur < end_exclusive) {
        uint64_t remain = end_exclusive - cur;
        uint64_t size = floor_pow2_u64(remain);
        while ((cur & (size - 1ULL)) != 0ULL) {
            size >>= 1U;
        }
        if (size == 0ULL) {
            break;
        }
        uint32_t scale_idx = 0U;
        uint64_t s = size;
        while (s > 1ULL) {
            s >>= 1U;
            ++scale_idx;
        }
        if (scale_idx > g_advanced_max_scale) {
            scale_idx = g_advanced_max_scale;
        }
        uint64_t hist_idx = advanced_hist_index(scale_idx, life_code);
        if (hist_idx < g_advanced_hist_size) {
            ++g_advanced_hist[hist_idx];
        }
        cur += size;
    }
}

static uint64_t max_advanced_group_buckets_locked(void) {
    if (g_mdts_bytes == 0ULL) {
        return g_bucket_count;
    }
    uint64_t b = g_mdts_bytes / NVME_POST_ACTION_STATS_BLOCK_BYTES;
    if (b == 0ULL) {
        b = 1ULL;
    }
    if (b > g_bucket_count) {
        b = g_bucket_count;
    }
    return b;
}

int nvme_post_action_stats_init(uint32_t sector_size) {
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_inited != 0) {
        if (sector_size != 0U) {
            g_sector_size = sector_size;
        }
        int enabled = g_stats_enabled;
        pthread_mutex_unlock(&g_stats_mutex);
        return enabled ? 0 : -1;
    }

    if (sector_size != 0U) {
        g_sector_size = sector_size;
    }
    build_latency_threshold_table();

    uint64_t bucket_count = NVME_POST_ACTION_DEFAULT_TOTAL_LBA_BYTES / NVME_POST_ACTION_STATS_BLOCK_BYTES;
    uint64_t total_bytes = bucket_count * NVME_POST_ACTION_STATS_ENTRY_BYTES;
    int flags = MAP_PRIVATE;
    int mmap_fd = -1;
#if defined(MAP_ANONYMOUS)
    flags |= MAP_ANONYMOUS;
#elif defined(MAP_ANON)
    flags |= MAP_ANON;
#else
    mmap_fd = open("/dev/zero", O_RDWR);
    if (mmap_fd < 0) {
        g_stats_enabled = 0;
        g_stats_inited = 1;
        pthread_mutex_unlock(&g_stats_mutex);
        return -1;
    }
#endif
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void *mem = mmap(NULL, (size_t)total_bytes, PROT_READ | PROT_WRITE, flags, mmap_fd, 0);
#if !defined(MAP_ANONYMOUS) && !defined(MAP_ANON)
    close(mmap_fd);
#endif
    if (mem == MAP_FAILED) {
        g_stats_enabled = 0;
        g_stats_inited = 1;
        pthread_mutex_unlock(&g_stats_mutex);
        return -1;
    }

    if (active_writes_init(1ULL << 20U) != 0) {
        munmap(mem, (size_t)total_bytes);
        g_stats_enabled = 0;
        g_stats_inited = 1;
        pthread_mutex_unlock(&g_stats_mutex);
        return -1;
    }

    uint32_t max_scale = 0U;
    uint64_t tmp = bucket_count;
    while (tmp > 1ULL) {
        tmp >>= 1U;
        ++max_scale;
    }
    uint64_t hist_size = ((uint64_t)max_scale + 1ULL) *
                         ((uint64_t)NVME_POST_ACTION_LATENCY_CODE_MAX + 1ULL);
    uint64_t *advanced_hist = (uint64_t *)calloc((size_t)hist_size, sizeof(uint64_t));
    if (advanced_hist == NULL) {
        munmap(mem, (size_t)total_bytes);
        free(g_active_writes);
        g_active_writes = NULL;
        g_active_writes_cap = 0ULL;
        g_active_writes_count = 0ULL;
        g_stats_enabled = 0;
        g_stats_inited = 1;
        pthread_mutex_unlock(&g_stats_mutex);
        return -1;
    }

    g_stats = (nvme_post_action_lba_stat_t *)mem;
    g_bucket_count = bucket_count;
    g_total_bytes = total_bytes;
    g_advanced_max_scale = max_scale;
    g_advanced_hist = advanced_hist;
    g_advanced_hist_size = hist_size;
    g_stat_samples = NULL;
    g_stat_samples_count = 0ULL;
    g_stat_samples_cap = 0ULL;
    g_stats_enabled = 1;
    g_stats_inited = 1;
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_set_mdts_bytes(uint64_t mdts_bytes) {
    pthread_mutex_lock(&g_stats_mutex);
    g_mdts_bytes = mdts_bytes;
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_get_advanced_max_scale(uint32_t *max_scale_out) {
    if (max_scale_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || g_advanced_hist == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    *max_scale_out = g_advanced_max_scale;
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_get_advanced_count(uint32_t scale_idx,
                                              uint16_t life_code,
                                              uint64_t *count_out) {
    if (count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || g_advanced_hist == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    if (scale_idx > g_advanced_max_scale) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ERANGE;
        return -1;
    }
    uint64_t idx = advanced_hist_index(scale_idx, life_code);
    if (idx >= g_advanced_hist_size) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ERANGE;
        return -1;
    }
    *count_out = g_advanced_hist[idx];
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

void nvme_post_action_stats_update_write(uint64_t start_lba,
                                         uint64_t len_lba,
                                         uint64_t abs_time_us) {
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || len_lba == 0ULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    uint64_t begin = 0ULL;
    uint64_t end_exclusive = 0ULL;
    if (stats_index_for_lba_range_locked(start_lba, len_lba, &begin, &end_exclusive) != 0) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    for (uint64_t idx = begin; idx < end_exclusive; ++idx) {
        nvme_post_action_lba_stat_t *s = &g_stats[idx];
        int is_new = 0;
        lba_active_write_entry_t *w = active_writes_get_or_insert(idx, &is_new);
        if (w == NULL) {
            continue;
        }
        if (!is_new && w->last_write_abs_us > 0ULL && abs_time_us >= w->last_write_abs_us) {
            uint32_t d_ms = duration_us_to_ms(abs_time_us - w->last_write_abs_us);
            uint16_t life_code = encode_duration_ms_non_linear(d_ms);
            s->life_cycle_latency = life_code;
            update_advanced_life_cycle_hist_locked(idx, idx + 1ULL, life_code);
        }
        w->last_write_abs_us = abs_time_us;
        w->first_read_seen = 0U;
    }
    pthread_mutex_unlock(&g_stats_mutex);
}

void nvme_post_action_stats_update_read(uint64_t start_lba,
                                        uint64_t len_lba,
                                        uint64_t abs_time_us) {
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || len_lba == 0ULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    uint64_t begin = 0ULL;
    uint64_t end_exclusive = 0ULL;
    if (stats_index_for_lba_range_locked(start_lba, len_lba, &begin, &end_exclusive) != 0) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    for (uint64_t idx = begin; idx < end_exclusive; ++idx) {
        nvme_post_action_lba_stat_t *s = &g_stats[idx];
        lba_active_write_entry_t *w = active_writes_lookup(idx);
        if (w == NULL || w->last_write_abs_us == 0ULL) {
            continue;
        }
        if (s->read_count < 255U) {
            ++s->read_count;
        }
        if (w->first_read_seen == 0U && abs_time_us >= w->last_write_abs_us) {
            uint32_t d_ms = duration_us_to_ms(abs_time_us - w->last_write_abs_us);
            s->write_to_first_read_latency = encode_duration_ms_non_linear(d_ms);
            w->first_read_seen = 1U;
        }
    }
    pthread_mutex_unlock(&g_stats_mutex);
}

void nvme_post_action_stats_print_summary_debug(int debug_enabled) {
    if (debug_enabled == 0) {
        return;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || g_stats == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    uint64_t touched = 0ULL;
    uint64_t non_zero_reads = 0ULL;
    for (uint64_t i = 0ULL; i < g_bucket_count; ++i) {
        const nvme_post_action_lba_stat_t *s = &g_stats[i];
        if (s->read_count != 0U || s->write_to_first_read_latency != 0U || s->life_cycle_latency != 0U) {
            ++touched;
        }
        if (s->read_count != 0U) {
            ++non_zero_reads;
        }
    }
    fprintf(stderr,
            "post action stats: buckets=%llu touched=%llu read_count_nonzero=%llu bytes=%llu\n",
            (unsigned long long)g_bucket_count,
            (unsigned long long)touched,
            (unsigned long long)non_zero_reads,
            (unsigned long long)g_total_bytes);
    if (g_advanced_hist != NULL) {
        fprintf(stderr, "advanced life-cycle stats (4K..):\n");
        for (uint32_t scale = 0U; scale <= g_advanced_max_scale; ++scale) {
            uint64_t total = 0ULL;
            for (uint32_t code = 0U; code <= NVME_POST_ACTION_LATENCY_CODE_MAX; ++code) {
                total += g_advanced_hist[advanced_hist_index(scale, (uint16_t)code)];
            }
            if (total == 0ULL) {
                continue;
            }
            uint64_t range_kib = 4ULL << scale;
            fprintf(stderr, "  range=%lluKiB count=%llu\n",
                    (unsigned long long)range_kib,
                    (unsigned long long)total);
        }
    }
    pthread_mutex_unlock(&g_stats_mutex);
}

void nvme_post_action_stats_advanced_record_overwrite(uint64_t start_lba,
                                                      uint64_t len_lba,
                                                      uint64_t abs_time_us) {
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || len_lba == 0ULL || g_advanced_hist == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    uint64_t begin = 0ULL;
    uint64_t end_exclusive = 0ULL;
    if (stats_index_for_lba_range_locked(start_lba, len_lba, &begin, &end_exclusive) != 0) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    uint64_t bucket_count = end_exclusive - begin;
    uint64_t max_group_buckets = max_advanced_group_buckets_locked();
    if (bucket_count > max_group_buckets) {
        bucket_count = max_group_buckets;
    }
    uint64_t aligned_count = floor_pow2_u64(bucket_count);
    if (aligned_count == 0ULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    uint64_t aligned_end = begin + aligned_count;

    uint16_t life_code = 0U;
    int all_same = 1;
    uint64_t first_prev_write = 0ULL;
    for (uint64_t idx = begin; idx < aligned_end; ++idx) {
        lba_active_write_entry_t *w = active_writes_lookup(idx);
        if (w == NULL || w->last_write_abs_us == 0ULL || abs_time_us < w->last_write_abs_us) {
            all_same = 0;
            break;
        }
        nvme_post_action_lba_stat_t *s = &g_stats[idx];
        uint32_t d_ms = duration_us_to_ms(abs_time_us - w->last_write_abs_us);
        uint16_t c = encode_duration_ms_non_linear(d_ms);
        if (idx == begin) {
            life_code = c;
            first_prev_write = w->last_write_abs_us;
        } else if (c != life_code) {
            all_same = 0;
            break;
        }
        (void)first_prev_write;
        (void)s;
    }

    if (all_same != 0) {
        update_advanced_life_cycle_hist_locked(begin, aligned_end, life_code);
    }
    pthread_mutex_unlock(&g_stats_mutex);
}

int nvme_post_action_stats_get_bucket_count(uint64_t *bucket_count_out) {
    if (bucket_count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || g_stats == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    *bucket_count_out = g_bucket_count;
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_get_bucket(uint64_t bucket_index,
                                      nvme_post_action_lba_stat_t *out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || g_stats == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    if (bucket_index >= g_bucket_count) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ERANGE;
        return -1;
    }
    *out = g_stats[bucket_index];
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_get_advanced_life_count(uint64_t range_kib,
                                                   uint16_t life_code,
                                                   uint64_t *count_out) {
    if (count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (range_kib == 0ULL) {
        errno = EINVAL;
        return -1;
    }
    if ((range_kib & (range_kib - 1ULL)) != 0ULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0 || g_advanced_hist == NULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }

    uint64_t buckets = range_kib / 4ULL;
    if (buckets == 0ULL || (buckets & (buckets - 1ULL)) != 0ULL) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = EINVAL;
        return -1;
    }
    uint32_t scale = 0U;
    while (buckets > 1ULL) {
        buckets >>= 1U;
        ++scale;
    }
    if (scale > g_advanced_max_scale) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ERANGE;
        return -1;
    }

    uint64_t idx = advanced_hist_index(scale, life_code);
    if (idx >= g_advanced_hist_size) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ERANGE;
        return -1;
    }
    *count_out = g_advanced_hist[idx];
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_record_stat(uint64_t abs_time_us,
                                       uint16_t qd,
                                       uint32_t hot_write_4k,
                                       uint32_t folding_write_4k) {
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    if (qd == 0U && hot_write_4k == 0U) {
        pthread_mutex_unlock(&g_stats_mutex);
        return 0;
    }
    if (stat_samples_ensure_capacity_locked(g_stat_samples_count + 1ULL) != 0) {
        int saved_errno = errno == 0 ? ENOMEM : errno;
        pthread_mutex_unlock(&g_stats_mutex);
        errno = saved_errno;
        return -1;
    }
    nvme_post_action_stat_sample_t *s = &g_stat_samples[g_stat_samples_count++];
    s->abs_time_us = abs_time_us;
    s->qd = qd;
    s->hot_write_4k = hot_write_4k;
    s->folding_write_4k = folding_write_4k;
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_get_stat_sample_count(uint64_t *count_out) {
    if (count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    *count_out = g_stat_samples_count;
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}

int nvme_post_action_stats_get_stat_sample(uint64_t sample_index,
                                           nvme_post_action_stat_sample_t *out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_stats_mutex);
    if (g_stats_enabled == 0) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ENODEV;
        return -1;
    }
    if (sample_index >= g_stat_samples_count) {
        pthread_mutex_unlock(&g_stats_mutex);
        errno = ERANGE;
        return -1;
    }
    *out = g_stat_samples[sample_index];
    pthread_mutex_unlock(&g_stats_mutex);
    return 0;
}
