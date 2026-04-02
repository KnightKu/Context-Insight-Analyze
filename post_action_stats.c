#define _POSIX_C_SOURCE 200809L
#include "post_action_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

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
