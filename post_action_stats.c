#define _POSIX_C_SOURCE 200809L
#include "post_action_stats.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

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
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void *mem = mmap(NULL, (size_t)total_bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
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

    g_stats = (nvme_post_action_lba_stat_t *)mem;
    g_bucket_count = bucket_count;
    g_total_bytes = total_bytes;
    g_stats_enabled = 1;
    g_stats_inited = 1;
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
            s->life_cycle_latency = encode_duration_ms_non_linear(d_ms);
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
