#include "post_action_latency.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    uint64_t count;
    uint64_t min_us;
    uint64_t max_us;
    uint64_t sum_us;
    uint64_t samples[1024];
    uint32_t sample_count;
} latency_bucket_t;

static pthread_mutex_t g_latency_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_latency_enabled = 0;
static latency_bucket_t g_latency_read = {.name = "read", .count = 0ULL, .min_us = UINT64_MAX, .max_us = 0ULL, .sum_us = 0ULL};
static latency_bucket_t g_latency_write = {.name = "write", .count = 0ULL, .min_us = UINT64_MAX, .max_us = 0ULL, .sum_us = 0ULL};
static latency_bucket_t g_latency_trim = {.name = "trim", .count = 0ULL, .min_us = UINT64_MAX, .max_us = 0ULL, .sum_us = 0ULL};

void nvme_post_action_latency_set_enabled(int enabled) {
    pthread_mutex_lock(&g_latency_mutex);
    g_latency_enabled = (enabled != 0) ? 1 : 0;
    pthread_mutex_unlock(&g_latency_mutex);
}

int nvme_post_action_latency_get_enabled(void) {
    pthread_mutex_lock(&g_latency_mutex);
    int enabled = g_latency_enabled;
    pthread_mutex_unlock(&g_latency_mutex);
    return enabled;
}

void nvme_post_action_latency_reset(void) {
    pthread_mutex_lock(&g_latency_mutex);
    g_latency_read.count = 0ULL;
    g_latency_read.min_us = UINT64_MAX;
    g_latency_read.max_us = 0ULL;
    g_latency_read.sum_us = 0ULL;
    g_latency_read.sample_count = 0U;

    g_latency_write.count = 0ULL;
    g_latency_write.min_us = UINT64_MAX;
    g_latency_write.max_us = 0ULL;
    g_latency_write.sum_us = 0ULL;
    g_latency_write.sample_count = 0U;

    g_latency_trim.count = 0ULL;
    g_latency_trim.min_us = UINT64_MAX;
    g_latency_trim.max_us = 0ULL;
    g_latency_trim.sum_us = 0ULL;
    g_latency_trim.sample_count = 0U;
    pthread_mutex_unlock(&g_latency_mutex);
}

static void latency_bucket_add(latency_bucket_t *bucket, uint32_t latency_us) {
    uint64_t v = (uint64_t)latency_us;
    ++bucket->count;
    bucket->sum_us += v;
    if (v < bucket->min_us) {
        bucket->min_us = v;
    }
    if (v > bucket->max_us) {
        bucket->max_us = v;
    }
    if (bucket->sample_count < 1024U) {
        bucket->samples[bucket->sample_count++] = v;
    }
}

static void sort_u64(uint64_t *arr, uint32_t n) {
    for (uint32_t i = 1U; i < n; ++i) {
        uint64_t key = arr[i];
        uint32_t j = i;
        while (j > 0U && arr[j - 1U] > key) {
            arr[j] = arr[j - 1U];
            --j;
        }
        arr[j] = key;
    }
}

static uint64_t percentile_from_sorted_ratio(const uint64_t *arr,
                                             uint32_t n,
                                             uint32_t numerator,
                                             uint32_t denominator) {
    if (n == 0U || arr == NULL || denominator == 0U) {
        return 0ULL;
    }
    if (numerator > denominator) {
        numerator = denominator;
    }
    uint64_t idx =
        ((uint64_t)(n - 1U) * (uint64_t)numerator) / (uint64_t)denominator;
    return arr[idx];
}

static void print_percentiles_line(const char *name,
                                   const uint64_t *sorted,
                                   uint32_t n) {
    fprintf(stderr, "lat(%s) pct(us):", name);
    for (uint32_t p = 10U; p <= 90U; p += 10U) {
        fprintf(stderr, " p%u=%" PRIu64, p,
                percentile_from_sorted_ratio(sorted, n, p, 100U));
    }
    fprintf(stderr,
            " p99=%" PRIu64 " p99.9=%" PRIu64 " p99.99=%" PRIu64 "\n",
            percentile_from_sorted_ratio(sorted, n, 99U, 100U),
            percentile_from_sorted_ratio(sorted, n, 999U, 1000U),
            percentile_from_sorted_ratio(sorted, n, 9999U, 10000U));
}

void nvme_post_action_latency_record_read(uint32_t latency_us) {
    pthread_mutex_lock(&g_latency_mutex);
    if (g_latency_enabled != 0) {
        latency_bucket_add(&g_latency_read, latency_us);
    }
    pthread_mutex_unlock(&g_latency_mutex);
}

void nvme_post_action_latency_record_write(uint32_t latency_us) {
    pthread_mutex_lock(&g_latency_mutex);
    if (g_latency_enabled != 0) {
        latency_bucket_add(&g_latency_write, latency_us);
    }
    pthread_mutex_unlock(&g_latency_mutex);
}

void nvme_post_action_latency_record_trim(uint32_t latency_us) {
    pthread_mutex_lock(&g_latency_mutex);
    if (g_latency_enabled != 0) {
        latency_bucket_add(&g_latency_trim, latency_us);
    }
    pthread_mutex_unlock(&g_latency_mutex);
}

static void print_bucket(const latency_bucket_t *bucket) {
    if (bucket->count == 0ULL) {
        fprintf(stderr, "lat(%s): count=0\n", bucket->name);
        print_percentiles_line(bucket->name, NULL, 0U);
        return;
    }
    double avg = (double)bucket->sum_us / (double)bucket->count;
    uint64_t sorted[1024];
    uint32_t sorted_n = 0U;
    if (bucket->sample_count > 0U) {
        memcpy(sorted, bucket->samples, bucket->sample_count * sizeof(uint64_t));
        sorted_n = bucket->sample_count;
        sort_u64(sorted, sorted_n);
    }
    fprintf(stderr,
            "lat(%s): count=%" PRIu64 " min=%" PRIu64 "us max=%" PRIu64 "us avg=%.2fus\n",
            bucket->name,
            bucket->count,
            bucket->min_us,
            bucket->max_us,
            avg);
    print_percentiles_line(bucket->name, sorted, sorted_n);
}

void nvme_post_action_latency_print_summary(void) {
    pthread_mutex_lock(&g_latency_mutex);
    int enabled = g_latency_enabled;
    latency_bucket_t read_copy = g_latency_read;
    latency_bucket_t write_copy = g_latency_write;
    latency_bucket_t trim_copy = g_latency_trim;
    pthread_mutex_unlock(&g_latency_mutex);

    if (enabled == 0) {
        return;
    }

    fprintf(stderr, "latency summary (fio-like):\n");
    print_bucket(&read_copy);
    print_bucket(&write_copy);
    print_bucket(&trim_copy);
}
