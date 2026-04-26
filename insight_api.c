#define _POSIX_C_SOURCE 200809L

#include "insight_api.h"

#include "nvme_read.h"
#include "post_action.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INSIGHT_SCAN_SLBA_BYTES 0ULL
#define INSIGHT_SCAN_DATA_LEN_11T_BYTES (11ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define INSIGHT_CAPTURE_MAX_BYTES (INSIGHT_JSON_BUFFER_BYTES * 4U)

typedef enum {
    INSIGHT_QUERY_LATENCY = 0,
    INSIGHT_QUERY_WA = 1,
    INSIGHT_QUERY_QD = 2,
    INSIGHT_QUERY_READ_SIZE = 3,
    INSIGHT_QUERY_WRITE_SIZE = 4,
    INSIGHT_QUERY_READ_THROUGHPUT = 5,
    INSIGHT_QUERY_WRITE_THROUGHPUT = 6,
    INSIGHT_QUERY_READ_COUNT = 7,
    INSIGHT_QUERY_W2FR = 8,
    INSIGHT_QUERY_LIFECYCLE = 9
} insight_query_type_t;

static int insight_query_is_lba_ratio(insight_query_type_t query_type) {
    return (query_type == INSIGHT_QUERY_READ_COUNT ||
            query_type == INSIGHT_QUERY_W2FR ||
            query_type == INSIGHT_QUERY_LIFECYCLE) ? 1 : 0;
}

static pthread_mutex_t g_insight_api_mutex = PTHREAD_MUTEX_INITIALIZER;

static int parse_datetime_ymdhms(const char *arg, uint64_t *out_ms) {
    if (arg == NULL || out_ms == NULL) {
        errno = EINVAL;
        return -1;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(arg, "%d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
        errno = EINVAL;
        return -1;
    }
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = second;
    tm_val.tm_isdst = -1;
    time_t ts = mktime(&tm_val);
    if (ts < 0) {
        errno = EINVAL;
        return -1;
    }
    *out_ms = (uint64_t)ts * 1000ULL;
    return 0;
}

static uint64_t insight_effective_scan_bytes(void) {
    uint64_t log_span_bytes = 0ULL;
    if (LOG_END_LBA > LOG_START_LBA) {
        log_span_bytes = LOG_END_LBA - LOG_START_LBA;
    }
    if (log_span_bytes == 0ULL) {
        return 0ULL;
    }
    return (INSIGHT_SCAN_DATA_LEN_11T_BYTES < log_span_bytes) ?
        INSIGHT_SCAN_DATA_LEN_11T_BYTES : log_span_bytes;
}

static int capture_stderr_content(FILE *src, char *out, size_t out_size) {
    if (src == NULL || out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    char *scratch = (char *)malloc(INSIGHT_CAPTURE_MAX_BYTES);
    if (scratch == NULL) {
        errno = ENOMEM;
        return -1;
    }

    size_t total = 0U;
    while (total < (INSIGHT_CAPTURE_MAX_BYTES - 1U)) {
        size_t room = (INSIGHT_CAPTURE_MAX_BYTES - 1U) - total;
        size_t got = fread(scratch + total, 1U, room, src);
        if (got == 0U) {
            break;
        }
        total += got;
    }
    scratch[total] = '\0';

    const char *begin = strchr(scratch, '{');
    const char *end = strrchr(scratch, '}');
    if (begin == NULL || end == NULL || end < begin) {
        free(scratch);
        errno = ENODATA;
        return -1;
    }

    size_t json_len = (size_t)(end - begin + 1);
    if (json_len >= out_size) {
        free(scratch);
        errno = ENOSPC;
        return -1;
    }
    memcpy(out, begin, json_len);
    out[json_len] = '\0';
    free(scratch);
    return 0;
}

static int run_query_and_fill_json(const char *device,
                                   uint64_t block_size,
                                   const char *time_start,
                                   const char *time_end,
                                   insight_query_type_t query_type,
                                   char *json_buffer) {
    if (device == NULL || json_buffer == NULL || block_size == 0ULL ||
        time_start == NULL || time_end == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint64_t start_ms = 0ULL;
    uint64_t end_ms = 0ULL;
    if (parse_datetime_ymdhms(time_start, &start_ms) != 0 ||
        parse_datetime_ymdhms(time_end, &end_ms) != 0 ||
        start_ms > end_ms) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_insight_api_mutex);
    json_buffer[0] = '\0';

    int enable_lba = insight_query_is_lba_ratio(query_type);
    if (nvme_read_set_debug(0) != 0 ||
        nvme_read_set_format_json(1) != 0 ||
        nvme_read_set_latency(query_type == INSIGHT_QUERY_LATENCY) != 0 ||
        nvme_read_set_lba_stats_read_count(enable_lba &&
                                           query_type == INSIGHT_QUERY_READ_COUNT) != 0 ||
        nvme_read_set_lba_stats_w2fr(enable_lba &&
                                     query_type == INSIGHT_QUERY_W2FR) != 0 ||
        nvme_read_set_lba_stats_life_cycle(enable_lba &&
                                           query_type == INSIGHT_QUERY_LIFECYCLE) != 0 ||
        nvme_read_set_qd_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_QD)) != 0 ||
        nvme_read_set_wa_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_WA)) != 0 ||
        nvme_read_set_read_size_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_READ_SIZE)) != 0 ||
        nvme_read_set_write_size_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_WRITE_SIZE)) != 0 ||
        nvme_read_set_read_throughput_dist(enable_lba ? 0 :
                                           (query_type == INSIGHT_QUERY_READ_THROUGHPUT)) != 0 ||
        nvme_read_set_write_throughput_dist(enable_lba ? 0 :
                                            (query_type == INSIGHT_QUERY_WRITE_THROUGHPUT)) != 0 ||
        nvme_read_set_trim_size_dist(0) != 0 ||
        nvme_read_set_block_size_bytes(block_size) != 0 ||
        nvme_read_set_time_window(1, start_ms, 1, end_ms) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&g_insight_api_mutex);
        errno = saved_errno;
        return -1;
    }

    nvme_post_action_reset_invalid_count();
    nvme_post_action_reset_latency_stats();
    nvme_post_action_reset_workload_stats();

    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&g_insight_api_mutex);
        errno = saved_errno;
        return -1;
    }

    FILE *capture_file = tmpfile();
    if (capture_file == NULL) {
        int saved_errno = errno;
        close(saved_stderr);
        pthread_mutex_unlock(&g_insight_api_mutex);
        errno = saved_errno;
        return -1;
    }

    int ret = 0;
    int saved_errno = 0;
    if (dup2(fileno(capture_file), STDERR_FILENO) < 0) {
        saved_errno = errno;
        ret = -1;
        goto out_capture;
    }

    uint64_t scan_bytes = insight_effective_scan_bytes();
    if (scan_bytes == 0ULL) {
        saved_errno = ERANGE;
        ret = -1;
        goto out_capture;
    }
    if (nvme_read(device, INSIGHT_SCAN_SLBA_BYTES, scan_bytes, NULL) != 0) {
        saved_errno = errno;
        ret = -1;
        goto out_capture;
    }

    fflush(stderr);
    if (fseek(capture_file, 0L, SEEK_SET) != 0) {
        saved_errno = errno;
        ret = -1;
        goto out_capture;
    }

    if (capture_stderr_content(capture_file, json_buffer, INSIGHT_JSON_BUFFER_BYTES) != 0) {
        saved_errno = errno;
        ret = -1;
        goto out_capture;
    }

out_capture:
    (void)fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0 && saved_errno == 0) {
        saved_errno = errno;
        ret = -1;
    }
    close(saved_stderr);
    fclose(capture_file);
    pthread_mutex_unlock(&g_insight_api_mutex);
    if (ret != 0) {
        errno = saved_errno == 0 ? EIO : saved_errno;
    }
    return ret;
}

int get_read_latency_percentiles(const char *device,
                                 uint64_t block_size,
                                 const char *time_start,
                                 const char *time_end,
                                 char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_LATENCY, json_buffer);
}

int get_write_amplification(const char *device,
                            uint64_t block_size,
                            const char *time_start,
                            const char *time_end,
                            char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_WA, json_buffer);
}

int get_qd_distribution(const char *device,
                        uint64_t block_size,
                        const char *time_start,
                        const char *time_end,
                        char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_QD, json_buffer);
}

int get_read_size_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_READ_SIZE, json_buffer);
}

int get_write_size_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_WRITE_SIZE, json_buffer);
}

int get_read_throughput_distribution(const char *device,
                                     uint64_t block_size,
                                     const char *time_start,
                                     const char *time_end,
                                     char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_READ_THROUGHPUT, json_buffer);
}

int get_write_throughput_distribution(const char *device,
                                      uint64_t block_size,
                                      const char *time_start,
                                      const char *time_end,
                                      char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_WRITE_THROUGHPUT, json_buffer);
}

int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_READ_COUNT, json_buffer);
}

int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_W2FR, json_buffer);
}

int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer) {
    return run_query_and_fill_json(device, block_size, time_start, time_end,
                                   INSIGHT_QUERY_LIFECYCLE, json_buffer);
}
