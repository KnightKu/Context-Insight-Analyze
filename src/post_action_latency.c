#include "post_action_latency.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
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

typedef struct {
    atomic_uint_fast64_t count;
    atomic_uint_fast64_t min_us;
    atomic_uint_fast64_t max_us;
    atomic_uint_fast64_t sum_us;
    uint64_t samples[1024];
    atomic_uint_fast32_t sample_count;
    const char *name;
} latency_live_bucket_t;

static pthread_mutex_t g_latency_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_latency_enabled = 0;
static atomic_int g_latency_enabled_fast = ATOMIC_VAR_INIT(0);
static int g_latency_json_enabled = 0;

static latency_live_bucket_t g_live_read = {
    .count = ATOMIC_VAR_INIT(0),
    .min_us = ATOMIC_VAR_INIT(UINT64_MAX),
    .max_us = ATOMIC_VAR_INIT(0),
    .sum_us = ATOMIC_VAR_INIT(0),
    .sample_count = ATOMIC_VAR_INIT(0),
    .name = "read",
};

static latency_live_bucket_t g_live_write = {
    .count = ATOMIC_VAR_INIT(0),
    .min_us = ATOMIC_VAR_INIT(UINT64_MAX),
    .max_us = ATOMIC_VAR_INIT(0),
    .sum_us = ATOMIC_VAR_INIT(0),
    .sample_count = ATOMIC_VAR_INIT(0),
    .name = "write",
};

static latency_live_bucket_t g_live_trim = {
    .count = ATOMIC_VAR_INIT(0),
    .min_us = ATOMIC_VAR_INIT(UINT64_MAX),
    .max_us = ATOMIC_VAR_INIT(0),
    .sum_us = ATOMIC_VAR_INIT(0),
    .sample_count = ATOMIC_VAR_INIT(0),
    .name = "trim",
};

static void latency_live_reset(latency_live_bucket_t *b) {
    atomic_store_explicit(&b->count, 0ULL, memory_order_relaxed);
    atomic_store_explicit(&b->min_us, UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&b->max_us, 0ULL, memory_order_relaxed);
    atomic_store_explicit(&b->sum_us, 0ULL, memory_order_relaxed);
    atomic_store_explicit(&b->sample_count, 0U, memory_order_relaxed);
}

static void latency_live_snapshot(const latency_live_bucket_t *live, latency_bucket_t *out) {
    out->name = live->name;
    out->count = atomic_load_explicit(&live->count, memory_order_relaxed);
    out->min_us = atomic_load_explicit(&live->min_us, memory_order_relaxed);
    out->max_us = atomic_load_explicit(&live->max_us, memory_order_relaxed);
    out->sum_us = atomic_load_explicit(&live->sum_us, memory_order_relaxed);
    uint32_t n = atomic_load_explicit(&live->sample_count, memory_order_relaxed);
    if (n > 1024U) {
        n = 1024U;
    }
    out->sample_count = n;
    if (n > 0U) {
        memcpy(out->samples, live->samples, (size_t)n * sizeof(uint64_t));
    }
}

static void latency_live_add(latency_live_bucket_t *b, uint32_t latency_us) {
    uint64_t v = (uint64_t)latency_us;
    atomic_fetch_add_explicit(&b->count, 1ULL, memory_order_relaxed);
    atomic_fetch_add_explicit(&b->sum_us, v, memory_order_relaxed);

    uint64_t cur_min = atomic_load_explicit(&b->min_us, memory_order_relaxed);
    while (v < cur_min) {
        if (atomic_compare_exchange_weak_explicit(&b->min_us,
                                                  &cur_min,
                                                  v,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
    }

    uint64_t cur_max = atomic_load_explicit(&b->max_us, memory_order_relaxed);
    while (v > cur_max) {
        if (atomic_compare_exchange_weak_explicit(&b->max_us,
                                                  &cur_max,
                                                  v,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
    }

    while (1) {
        uint_fast32_t idx = atomic_load_explicit(&b->sample_count, memory_order_relaxed);
        if (idx >= 1024U) {
            break;
        }
        uint_fast32_t next = idx + 1U;
        if (atomic_compare_exchange_weak_explicit(&b->sample_count,
                                                  &idx,
                                                  next,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            b->samples[idx] = v;
            break;
        }
    }
}

void nvme_post_action_latency_set_enabled(int enabled) {
    int v = (enabled != 0) ? 1 : 0;
    pthread_mutex_lock(&g_latency_mutex);
    g_latency_enabled = v;
    atomic_store_explicit(&g_latency_enabled_fast, v, memory_order_release);
    pthread_mutex_unlock(&g_latency_mutex);
}

int nvme_post_action_latency_get_enabled(void) {
    pthread_mutex_lock(&g_latency_mutex);
    int enabled = g_latency_enabled;
    pthread_mutex_unlock(&g_latency_mutex);
    return enabled;
}

void nvme_post_action_latency_set_json_format(int enabled) {
    pthread_mutex_lock(&g_latency_mutex);
    g_latency_json_enabled = (enabled != 0) ? 1 : 0;
    pthread_mutex_unlock(&g_latency_mutex);
}

int nvme_post_action_latency_get_json_format(void) {
    pthread_mutex_lock(&g_latency_mutex);
    int enabled = g_latency_json_enabled;
    pthread_mutex_unlock(&g_latency_mutex);
    return enabled;
}

void nvme_post_action_latency_reset(void) {
    pthread_mutex_lock(&g_latency_mutex);
    latency_live_reset(&g_live_read);
    latency_live_reset(&g_live_write);
    latency_live_reset(&g_live_trim);
    pthread_mutex_unlock(&g_latency_mutex);
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
    fprintf(stderr, "%s (us):\n Qos", name);
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
    if (atomic_load_explicit(&g_latency_enabled_fast, memory_order_acquire) == 0) {
        return;
    }
    latency_live_add(&g_live_read, latency_us);
}

void nvme_post_action_latency_record_write(uint32_t latency_us) {
    if (atomic_load_explicit(&g_latency_enabled_fast, memory_order_acquire) == 0) {
        return;
    }
    latency_live_add(&g_live_write, latency_us);
}

void nvme_post_action_latency_record_trim(uint32_t latency_us) {
    if (atomic_load_explicit(&g_latency_enabled_fast, memory_order_acquire) == 0) {
        return;
    }
    latency_live_add(&g_live_trim, latency_us);
}

static void print_bucket(const latency_bucket_t *bucket) {
    if (bucket->count == 0ULL) {
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
            "lat(%s): min=%" PRIu64 "us max=%" PRIu64 "us avg=%.2fus\n",
            bucket->name,
            bucket->min_us,
            bucket->max_us,
            avg);
    print_percentiles_line(bucket->name, sorted, sorted_n);
}

static void print_bucket_json(const latency_bucket_t *bucket, int *printed_any) {
    if (bucket->count == 0ULL || printed_any == NULL) {
        return;
    }
    uint64_t sorted[1024];
    uint32_t sorted_n = 0U;
    if (bucket->sample_count > 0U) {
        memcpy(sorted, bucket->samples, bucket->sample_count * sizeof(uint64_t));
        sorted_n = bucket->sample_count;
        sort_u64(sorted, sorted_n);
    }
    if (*printed_any != 0) {
        fprintf(stderr, ",\n");
    }
    double avg = (double)bucket->sum_us / (double)bucket->count;
    fprintf(stderr,
            "    \"%s\": {\n"
            "      \"count\": %" PRIu64 ",\n"
            "      \"min_us\": %" PRIu64 ",\n"
            "      \"max_us\": %" PRIu64 ",\n"
            "      \"avg_us\": %.2f,\n"
            "      \"percentiles_us\": {\n"
            "        \"p10\": %" PRIu64 ",\n"
            "        \"p20\": %" PRIu64 ",\n"
            "        \"p30\": %" PRIu64 ",\n"
            "        \"p40\": %" PRIu64 ",\n"
            "        \"p50\": %" PRIu64 ",\n"
            "        \"p60\": %" PRIu64 ",\n"
            "        \"p70\": %" PRIu64 ",\n"
            "        \"p80\": %" PRIu64 ",\n"
            "        \"p90\": %" PRIu64 ",\n"
            "        \"p99\": %" PRIu64 ",\n"
            "        \"p99_9\": %" PRIu64 ",\n"
            "        \"p99_99\": %" PRIu64 "\n"
            "      }\n"
            "    }",
            bucket->name,
            bucket->count,
            bucket->min_us,
            bucket->max_us,
            avg,
            percentile_from_sorted_ratio(sorted, sorted_n, 10U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 20U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 30U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 40U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 50U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 60U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 70U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 80U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 90U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 99U, 100U),
            percentile_from_sorted_ratio(sorted, sorted_n, 999U, 1000U),
            percentile_from_sorted_ratio(sorted, sorted_n, 9999U, 10000U));
    *printed_any = 1;
}

void nvme_post_action_latency_print_summary(void) {
    pthread_mutex_lock(&g_latency_mutex);
    int enabled = g_latency_enabled;
    int json_enabled = g_latency_json_enabled;
    latency_bucket_t read_copy;
    latency_bucket_t write_copy;
    latency_bucket_t trim_copy;
    latency_live_snapshot(&g_live_read, &read_copy);
    latency_live_snapshot(&g_live_write, &write_copy);
    latency_live_snapshot(&g_live_trim, &trim_copy);
    pthread_mutex_unlock(&g_latency_mutex);

    if (enabled == 0) {
        return;
    }
    if (json_enabled != 0) {
        int printed_any = 0;
        fprintf(stderr, "{\n");
        fprintf(stderr, "  \"latency_percentiles\": {\n");
        print_bucket_json(&read_copy, &printed_any);
        print_bucket_json(&write_copy, &printed_any);
        print_bucket_json(&trim_copy, &printed_any);
        if (printed_any != 0) {
            fputc('\n', stderr);
        }
        fprintf(stderr, "  }\n");
        fprintf(stderr, "}\n");
        return;
    }
    fprintf(stderr, "latency summary:\n");
    print_bucket(&read_copy);
    print_bucket(&write_copy);
    print_bucket(&trim_copy);
}
