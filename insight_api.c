#define _POSIX_C_SOURCE 200809L

#include "insight_api.h"

#include "nvme_read.h"
#include "post_action.h"
#include "post_action_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INSIGHT_SCAN_SLBA_BYTES 0ULL
#define INSIGHT_SCAN_DATA_LEN_11T_BYTES (11ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define INSIGHT_CAPTURE_MAX_BYTES (INSIGHT_JSON_BUFFER_BYTES * 4U)

#ifndef INSIGHT_API_PERF_DEBUG
#define INSIGHT_API_PERF_DEBUG 0
#endif

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
    INSIGHT_QUERY_LIFECYCLE = 9,
    INSIGHT_QUERY_NAND_WRITE_VOLUME = 10,
    INSIGHT_QUERY_GC_DATA_MOVEMENT = 11
} insight_query_type_t;

static int insight_query_is_lba_ratio(insight_query_type_t query_type) {
    return (query_type == INSIGHT_QUERY_READ_COUNT ||
            query_type == INSIGHT_QUERY_W2FR ||
            query_type == INSIGHT_QUERY_LIFECYCLE) ? 1 : 0;
}

static int insight_query_is_stat_volume(insight_query_type_t query_type) {
    return (query_type == INSIGHT_QUERY_NAND_WRITE_VOLUME ||
            query_type == INSIGHT_QUERY_GC_DATA_MOVEMENT) ? 1 : 0;
}

static int insight_query_allow_zero_block_size(insight_query_type_t query_type) {
    return (query_type == INSIGHT_QUERY_LATENCY ||
            query_type == INSIGHT_QUERY_WA ||
            query_type == INSIGHT_QUERY_QD ||
            query_type == INSIGHT_QUERY_READ_SIZE ||
            query_type == INSIGHT_QUERY_WRITE_SIZE ||
            query_type == INSIGHT_QUERY_WRITE_THROUGHPUT ||
            query_type == INSIGHT_QUERY_READ_THROUGHPUT) ? 1 : 0;
}

static int insight_query_total_key_should_rename(insight_query_type_t query_type) {
    return (query_type == INSIGHT_QUERY_QD ||
            query_type == INSIGHT_QUERY_READ_SIZE ||
            query_type == INSIGHT_QUERY_WRITE_SIZE ||
            query_type == INSIGHT_QUERY_READ_THROUGHPUT ||
            query_type == INSIGHT_QUERY_WRITE_THROUGHPUT ||
            query_type == INSIGHT_QUERY_READ_COUNT ||
            query_type == INSIGHT_QUERY_W2FR ||
            query_type == INSIGHT_QUERY_LIFECYCLE) ? 1 : 0;
}

static pthread_mutex_t g_insight_api_mutex = PTHREAD_MUTEX_INITIALIZER;

#if INSIGHT_API_PERF_DEBUG
static uint64_t insight_monotonic_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0ULL;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static const char *insight_query_type_name(insight_query_type_t query_type) {
    switch (query_type) {
        case INSIGHT_QUERY_LATENCY:
            return "latency";
        case INSIGHT_QUERY_WA:
            return "write_amplification";
        case INSIGHT_QUERY_QD:
            return "qd_distribution";
        case INSIGHT_QUERY_READ_SIZE:
            return "read_size_distribution";
        case INSIGHT_QUERY_WRITE_SIZE:
            return "write_size_distribution";
        case INSIGHT_QUERY_READ_THROUGHPUT:
            return "read_throughput_distribution";
        case INSIGHT_QUERY_WRITE_THROUGHPUT:
            return "write_throughput_distribution";
        case INSIGHT_QUERY_READ_COUNT:
            return "read_count_distribution";
        case INSIGHT_QUERY_W2FR:
            return "write_to_first_read_distribution";
        case INSIGHT_QUERY_LIFECYCLE:
            return "lifecycle_distribution";
        case INSIGHT_QUERY_NAND_WRITE_VOLUME:
            return "nand_write_volume";
        case INSIGHT_QUERY_GC_DATA_MOVEMENT:
            return "gc_data_movement";
        default:
            return "unknown";
    }
}
#endif

static int run_query_and_fill_json(const char *device,
                                   uint64_t block_size,
                                   const char *time_start,
                                   const char *time_end,
                                   uint64_t lba_start,
                                   uint64_t lba_end,
                                   insight_query_type_t query_type,
                                   char *json_buffer);

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

static int insight_env_enabled_exact_one(const char *name) {
    const char *value = getenv(name);
    return (value != NULL && strcmp(value, "1") == 0) ? 1 : 0;
}

static void forward_prefixed_lines_to_stderr(const char *text, const char *prefix) {
    if (text == NULL || prefix == NULL || *prefix == '\0') {
        return;
    }
    size_t prefix_len = strlen(prefix);
    const char *line = text;
    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        size_t line_len = (line_end != NULL) ? (size_t)(line_end - line) : strlen(line);
        if (line_len >= prefix_len && strncmp(line, prefix, prefix_len) == 0) {
            fprintf(stderr, "%.*s\n", (int)line_len, line);
        }
        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
    }
}

static int append_json_escaped_string(char *dst, size_t dst_size, size_t *cursor, const char *src) {
    if (dst == NULL || cursor == NULL || src == NULL || dst_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        const char *esc = NULL;
        switch (*p) {
            case '\"':
                esc = "\\\"";
                break;
            case '\\':
                esc = "\\\\";
                break;
            case '\b':
                esc = "\\b";
                break;
            case '\f':
                esc = "\\f";
                break;
            case '\n':
                esc = "\\n";
                break;
            case '\r':
                esc = "\\r";
                break;
            case '\t':
                esc = "\\t";
                break;
            default:
                break;
        }
        if (esc != NULL) {
            size_t n = strlen(esc);
            if ((*cursor + n) >= dst_size) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(dst + *cursor, esc, n);
            *cursor += n;
            continue;
        }
        if (*p < 0x20U) {
            if ((*cursor + 6U) >= dst_size) {
                errno = ENOSPC;
                return -1;
            }
            int n = snprintf(dst + *cursor, dst_size - *cursor, "\\u%04x", (unsigned int)*p);
            if (n < 0 || (size_t)n >= (dst_size - *cursor)) {
                errno = ENOSPC;
                return -1;
            }
            *cursor += (size_t)n;
            continue;
        }
        if ((*cursor + 1U) >= dst_size) {
            errno = ENOSPC;
            return -1;
        }
        dst[*cursor] = (char)*p;
        *cursor += 1U;
    }
    return 0;
}

static int compose_query_result_json(const char *api_name,
                                     const char *device,
                                     int include_block_size,
                                     uint64_t block_size,
                                     const char *time_start,
                                     const char *time_end,
                                     const char *result_json,
                                     char *json_buffer) {
    if (api_name == NULL || device == NULL || time_start == NULL || time_end == NULL ||
        result_json == NULL || json_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t pos = 0U;
    int n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                     "{\n"
                     "  \"query\": {\n"
                     "    \"api\": \"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (append_json_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, api_name) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "\",\n"
                     "    \"device\": \"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (append_json_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, device) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 include_block_size != 0 ?
                     "\",\n"
                     "    \"block_size\": %llu,\n"
                     "    \"time_start\": \"" :
                     "\",\n"
                     "    \"time_start\": \"",
                 (unsigned long long)block_size);
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (append_json_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, time_start) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "\",\n"
                 "    \"time_end\": \"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (append_json_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, time_end) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "\"\n"
                 "  },\n"
                 "  \"result\": ");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    size_t result_len = strlen(result_json);
    if ((pos + result_len + 3U) >= INSIGHT_JSON_BUFFER_BYTES) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(json_buffer + pos, result_json, result_len);
    pos += result_len;
    json_buffer[pos++] = '\n';
    json_buffer[pos++] = '}';
    json_buffer[pos] = '\0';
    return 0;
}

static void skip_json_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        ++(*p);
    }
}

static int flatten_single_root_object(const char *input_json,
                                      char *flattened_json,
                                      size_t flattened_json_size) {
    if (input_json == NULL || flattened_json == NULL || flattened_json_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    const char *p = input_json;
    skip_json_ws(&p);
    if (*p != '{') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    skip_json_ws(&p);
    if (*p != '\"') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
            continue;
        }
        if (*p == '\"') {
            ++p;
            break;
        }
        ++p;
    }
    if (*(p - 1) != '\"') {
        errno = EINVAL;
        return -1;
    }
    skip_json_ws(&p);
    if (*p != ':') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    skip_json_ws(&p);
    if (*p != '{') {
        errno = EINVAL;
        return -1;
    }
    const char *value_start = p;
    int depth = 0;
    while (*p != '\0') {
        if (*p == '\"') {
            ++p;
            while (*p != '\0') {
                if (*p == '\\' && p[1] != '\0') {
                    p += 2;
                    continue;
                }
                if (*p == '\"') {
                    ++p;
                    break;
                }
                ++p;
            }
            continue;
        }
        if (*p == '{') {
            ++depth;
        } else if (*p == '}') {
            --depth;
            if (depth == 0) {
                ++p;
                break;
            }
        }
        ++p;
    }
    if (depth != 0) {
        errno = EINVAL;
        return -1;
    }
    const char *value_end = p;
    skip_json_ws(&p);
    if (*p != '}') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    skip_json_ws(&p);
    if (*p != '\0') {
        errno = EINVAL;
        return -1;
    }
    size_t out_len = (size_t)(value_end - value_start);
    if (out_len >= flattened_json_size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(flattened_json, value_start, out_len);
    flattened_json[out_len] = '\0';
    return 0;
}

static int compose_write_amplification_result_json(double wa_value,
                                                   char *result_json,
                                                   size_t result_json_size) {
    if (result_json == NULL || result_json_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(result_json, result_json_size,
                     "{\n"
                     "  \"write_amplification\": %.6f\n"
                     "}",
                     wa_value);
    if (n < 0 || (size_t)n >= result_json_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int extract_write_amplification_from_samples(const char *device,
                                                    uint64_t block_size,
                                                    const char *time_start,
                                                    const char *time_end,
                                                    uint64_t lba_start,
                                                    uint64_t lba_end,
                                                    char *json_buffer) {
    if (device == NULL || time_start == NULL || time_end == NULL ||
        json_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    char scratch_json[INSIGHT_JSON_BUFFER_BYTES];
    if (run_query_and_fill_json(device, block_size, time_start, time_end,
                                lba_start, lba_end,
                                INSIGHT_QUERY_WA, scratch_json) != 0) {
        return -1;
    }
    (void)scratch_json;

    uint64_t sample_count = 0ULL;
    if (nvme_post_action_stats_get_stat_sample_count(&sample_count) != 0) {
        return -1;
    }

    uint64_t sum_hot_4k = 0ULL;
    uint64_t sum_folding_4k = 0ULL;
    for (uint64_t i = 0ULL; i < sample_count; ++i) {
        nvme_post_action_stat_sample_t sample;
        if (nvme_post_action_stats_get_stat_sample(i, &sample) != 0) {
            return -1;
        }
        if (UINT64_MAX - sum_hot_4k < (uint64_t)sample.hot_write_4k ||
            UINT64_MAX - sum_folding_4k < (uint64_t)sample.folding_write_4k) {
            errno = ERANGE;
            return -1;
        }
        sum_hot_4k += (uint64_t)sample.hot_write_4k;
        sum_folding_4k += (uint64_t)sample.folding_write_4k;
    }

    double wa_value = 0.0;
    if (sum_hot_4k != 0ULL) {
        wa_value = ((double)sum_hot_4k + (double)sum_folding_4k) / (double)sum_hot_4k;
    }
    char result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (compose_write_amplification_result_json(wa_value, result_json, sizeof(result_json)) != 0) {
        return -1;
    }
    return compose_query_result_json("get_write_amplification",
                                     device,
                                     0,
                                     block_size,
                                     time_start,
                                     time_end,
                                     result_json,
                                     json_buffer);
}

static int compose_stat_volume_result_json(double volume_mib,
                                           char *result_json,
                                           size_t result_json_size) {
    if (result_json == NULL || result_json_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(result_json, result_json_size,
                     "{\n"
                     "  \"total\": \"%.2f MiB\"\n"
                     "}",
                     volume_mib);
    if (n < 0 || (size_t)n >= result_json_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int extract_stat_volume_from_samples(const char *device,
                                            uint64_t block_size,
                                            const char *time_start,
                                            const char *time_end,
                                            uint64_t lba_start,
                                            uint64_t lba_end,
                                            insight_query_type_t query_type,
                                            char *json_buffer) {
    if (device == NULL || time_start == NULL || time_end == NULL || json_buffer == NULL ||
        insight_query_is_stat_volume(query_type) == 0) {
        errno = EINVAL;
        return -1;
    }
    char scratch_json[INSIGHT_JSON_BUFFER_BYTES];
    if (run_query_and_fill_json(device, block_size, time_start, time_end,
                                lba_start, lba_end,
                                query_type, scratch_json) != 0) {
        return -1;
    }
    (void)scratch_json;

    uint64_t sample_count = 0ULL;
    if (nvme_post_action_stats_get_stat_sample_count(&sample_count) != 0) {
        return -1;
    }
    uint64_t sum_4k = 0ULL;
    for (uint64_t i = 0ULL; i < sample_count; ++i) {
        nvme_post_action_stat_sample_t sample;
        if (nvme_post_action_stats_get_stat_sample(i, &sample) != 0) {
            return -1;
        }
        if (query_type == INSIGHT_QUERY_NAND_WRITE_VOLUME) {
            sum_4k += (uint64_t)sample.hot_write_4k;
        } else {
            sum_4k += (uint64_t)sample.folding_write_4k;
        }
    }
    double volume_mib = (double)sum_4k * 4.0 / 1024.0;
    const char *api_name = (query_type == INSIGHT_QUERY_NAND_WRITE_VOLUME) ?
        "get_nand_write_volume" : "get_gc_data_movement";
    char result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (compose_stat_volume_result_json(volume_mib, result_json, sizeof(result_json)) != 0) {
        return -1;
    }
    return compose_query_result_json(api_name,
                                     device,
                                     0,
                                     0ULL,
                                     time_start,
                                     time_end,
                                     result_json,
                                     json_buffer);
}

static int run_query_and_fill_wrapped_json(const char *device,
                                           uint64_t block_size,
                                           const char *time_start,
                                           const char *time_end,
                                           uint64_t lba_start,
                                           uint64_t lba_end,
                                           insight_query_type_t query_type,
                                           const char *api_name,
                                           char *json_buffer) {
    if (api_name == NULL || json_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    char raw_result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (run_query_and_fill_json(device, block_size, time_start, time_end,
                                lba_start, lba_end,
                                query_type, raw_result_json) != 0) {
        return -1;
    }
    char flattened_result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (flatten_single_root_object(raw_result_json, flattened_result_json,
                                   sizeof(flattened_result_json)) != 0) {
        return -1;
    }
    if (insight_query_total_key_should_rename(query_type) != 0) {
        static const char old_key[] = "\"total\":";
        static const char new_key[] = "\"sampling\":";
        char *p = strstr(flattened_result_json, old_key);
        if (p != NULL) {
            size_t old_len = sizeof(old_key) - 1U;
            size_t new_len = sizeof(new_key) - 1U;
            size_t cur_len = strlen(flattened_result_json);
            if (new_len > old_len) {
                size_t growth = new_len - old_len;
                if (cur_len + growth >= sizeof(flattened_result_json)) {
                    errno = ENOSPC;
                    return -1;
                }
                memmove(p + new_len,
                        p + old_len,
                        (cur_len - (size_t)(p - flattened_result_json) - old_len) + 1U);
            } else if (new_len < old_len) {
                memmove(p + new_len,
                        p + old_len,
                        (cur_len - (size_t)(p - flattened_result_json) - old_len) + 1U);
            }
            memcpy(p, new_key, new_len);
        }
    }
    return compose_query_result_json(api_name,
                                     device,
                                     (block_size != 0ULL) ? 1 : 0,
                                     block_size,
                                     time_start,
                                     time_end,
                                     flattened_result_json,
                                     json_buffer);
}

static int extract_latency_bucket_result(const char *device,
                                         const char *time_start,
                                         const char *time_end,
                                         uint64_t lba_start,
                                         uint64_t lba_end,
                                         const char *api_name,
                                         const char *bucket_key,
                                         char *json_buffer) {
    if (device == NULL || time_start == NULL || time_end == NULL ||
        api_name == NULL || bucket_key == NULL || json_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    char raw_result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (run_query_and_fill_json(device, 0ULL, time_start, time_end,
                                lba_start, lba_end,
                                INSIGHT_QUERY_LATENCY, raw_result_json) != 0) {
        return -1;
    }
    char flattened_result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (flatten_single_root_object(raw_result_json, flattened_result_json,
                                   sizeof(flattened_result_json)) != 0) {
        return -1;
    }
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", bucket_key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) {
        errno = EINVAL;
        return -1;
    }
    char *start = strstr(flattened_result_json, needle);
    if (start == NULL) {
        errno = ENODATA;
        return -1;
    }
    char *obj_start = strchr(start, '{');
    if (obj_start == NULL) {
        errno = ENODATA;
        return -1;
    }
    int depth = 0;
    char *p = obj_start;
    while (*p != '\0') {
        if (*p == '\"') {
            ++p;
            while (*p != '\0') {
                if (*p == '\\' && p[1] != '\0') {
                    p += 2;
                    continue;
                }
                if (*p == '\"') {
                    ++p;
                    break;
                }
                ++p;
            }
            continue;
        }
        if (*p == '{') {
            ++depth;
        } else if (*p == '}') {
            --depth;
            if (depth == 0) {
                ++p;
                break;
            }
        }
        ++p;
    }
    if (depth != 0) {
        errno = ENODATA;
        return -1;
    }
    size_t bucket_len = (size_t)(p - obj_start);
    char result_json[INSIGHT_JSON_BUFFER_BYTES];
    if (bucket_len + 1U >= sizeof(result_json)) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(result_json, obj_start, bucket_len);
    result_json[bucket_len] = '\0';
    return compose_query_result_json(api_name,
                                     device,
                                     0,
                                     0ULL,
                                     time_start,
                                     time_end,
                                     result_json,
                                     json_buffer);
}

static int run_query_and_fill_json(const char *device,
                                   uint64_t block_size,
                                   const char *time_start,
                                   const char *time_end,
                                   uint64_t lba_start,
                                   uint64_t lba_end,
                                   insight_query_type_t query_type,
                                   char *json_buffer) {
    int allow_zero_block = insight_query_allow_zero_block_size(query_type);
    if (device == NULL || json_buffer == NULL ||
        (block_size == 0ULL && allow_zero_block == 0) ||
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
    if ((lba_start != 0ULL || lba_end != 0ULL) && lba_start > lba_end) {
        errno = EINVAL;
        return -1;
    }

#if INSIGHT_API_PERF_DEBUG
    uint64_t t_query_begin = insight_monotonic_now_ns();
    uint64_t t_after_config = 0ULL;
    uint64_t t_after_reset = 0ULL;
    uint64_t t_after_redirect = 0ULL;
    uint64_t t_after_read = 0ULL;
    uint64_t t_after_capture = 0ULL;
    uint64_t t_after_restore = 0ULL;
    nvme_read_perf_stats_t nvme_perf;
    memset(&nvme_perf, 0, sizeof(nvme_perf));
#endif

    pthread_mutex_lock(&g_insight_api_mutex);
    json_buffer[0] = '\0';
    int emit_post_action_perf = insight_env_enabled_exact_one("NVME_POST_ACTION_PERF");
    char *captured_stderr = NULL;
    if (emit_post_action_perf != 0) {
        captured_stderr = (char *)malloc(INSIGHT_CAPTURE_MAX_BYTES);
        if (captured_stderr == NULL) {
            emit_post_action_perf = 0;
        } else {
            captured_stderr[0] = '\0';
        }
    }

    int enable_lba = insight_query_is_lba_ratio(query_type);
    int enable_stat_volume = insight_query_is_stat_volume(query_type);
    int collect_stat_samples = (query_type == INSIGHT_QUERY_WA ||
                              query_type == INSIGHT_QUERY_NAND_WRITE_VOLUME ||
                              query_type == INSIGHT_QUERY_GC_DATA_MOVEMENT) ? 1 : 0;
    if (nvme_read_set_debug(0) != 0 ||
        nvme_read_set_format_json(1) != 0 ||
        nvme_read_set_stat_sample_collection(collect_stat_samples) != 0 ||
        nvme_read_set_latency(query_type == INSIGHT_QUERY_LATENCY) != 0 ||
        nvme_read_set_lba_stats_read_count(enable_lba &&
                                           query_type == INSIGHT_QUERY_READ_COUNT) != 0 ||
        nvme_read_set_lba_stats_w2fr(enable_lba &&
                                     query_type == INSIGHT_QUERY_W2FR) != 0 ||
        nvme_read_set_lba_stats_life_cycle(enable_lba &&
                                           query_type == INSIGHT_QUERY_LIFECYCLE) != 0 ||
        nvme_read_set_qd_dist(enable_stat_volume ? 1 :
                              (enable_lba ? 0 : (query_type == INSIGHT_QUERY_QD))) != 0 ||
        nvme_read_set_wa_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_WA)) != 0 ||
        nvme_read_set_read_size_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_READ_SIZE)) != 0 ||
        nvme_read_set_write_size_dist(enable_lba ? 0 : (query_type == INSIGHT_QUERY_WRITE_SIZE)) != 0 ||
        nvme_read_set_read_throughput_dist(enable_lba ? 0 :
                                           (query_type == INSIGHT_QUERY_READ_THROUGHPUT)) != 0 ||
        nvme_read_set_write_throughput_dist(enable_lba ? 0 :
                                            (query_type == INSIGHT_QUERY_WRITE_THROUGHPUT)) != 0 ||
        nvme_read_set_trim_size_dist(0) != 0 ||
        nvme_read_set_block_size_bytes(block_size) != 0 ||
        nvme_read_set_time_window(1, start_ms, 1, end_ms) != 0 ||
        nvme_read_set_lba_range_filter(lba_start, lba_end) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&g_insight_api_mutex);
        free(captured_stderr);
        errno = saved_errno;
        return -1;
    }
#if INSIGHT_API_PERF_DEBUG
    t_after_config = insight_monotonic_now_ns();
#endif

    nvme_post_action_reset_invalid_count();
    nvme_post_action_reset_latency_stats();
    nvme_post_action_reset_workload_stats();
    nvme_post_action_stats_reset_stat_samples();
#if INSIGHT_API_PERF_DEBUG
    t_after_reset = insight_monotonic_now_ns();
#endif

    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) {
        int saved_errno = errno;
        (void)nvme_read_set_lba_range_filter(0ULL, 0ULL);
        pthread_mutex_unlock(&g_insight_api_mutex);
        free(captured_stderr);
        errno = saved_errno;
        return -1;
    }

    FILE *capture_file = tmpfile();
    if (capture_file == NULL) {
        int saved_errno = errno;
        close(saved_stderr);
        (void)nvme_read_set_lba_range_filter(0ULL, 0ULL);
        pthread_mutex_unlock(&g_insight_api_mutex);
        free(captured_stderr);
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
#if INSIGHT_API_PERF_DEBUG
    t_after_redirect = insight_monotonic_now_ns();
#endif

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
#if INSIGHT_API_PERF_DEBUG
    t_after_read = insight_monotonic_now_ns();
    nvme_read_perf_get(&nvme_perf);
#endif

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
    if (emit_post_action_perf != 0 && captured_stderr != NULL) {
        if (fseek(capture_file, 0L, SEEK_SET) == 0) {
            size_t got = fread(captured_stderr, 1U, INSIGHT_CAPTURE_MAX_BYTES - 1U, capture_file);
            captured_stderr[got] = '\0';
        } else {
            captured_stderr[0] = '\0';
        }
    }
#if INSIGHT_API_PERF_DEBUG
    t_after_capture = insight_monotonic_now_ns();
#endif

out_capture:
    (void)nvme_read_set_lba_range_filter(0ULL, 0ULL);
    (void)fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0 && saved_errno == 0) {
        saved_errno = errno;
        ret = -1;
    }
    close(saved_stderr);
    fclose(capture_file);
    pthread_mutex_unlock(&g_insight_api_mutex);
#if INSIGHT_API_PERF_DEBUG
    t_after_restore = insight_monotonic_now_ns();
#endif
    if (emit_post_action_perf != 0 && captured_stderr != NULL) {
        forward_prefixed_lines_to_stderr(captured_stderr, "[post-action-perf]");
    }
    free(captured_stderr);
    if (ret != 0) {
        errno = saved_errno == 0 ? EIO : saved_errno;
    }
#if INSIGHT_API_PERF_DEBUG
    if (query_type == INSIGHT_QUERY_LATENCY) {
        uint64_t total_ns = (t_query_begin != 0ULL && t_after_restore >= t_query_begin) ?
            (t_after_restore - t_query_begin) : 0ULL;
        uint64_t config_ns = (t_after_config >= t_query_begin) ? (t_after_config - t_query_begin) : 0ULL;
        uint64_t reset_ns = (t_after_reset >= t_after_config) ? (t_after_reset - t_after_config) : 0ULL;
        uint64_t redirect_ns = (t_after_redirect >= t_after_reset) ? (t_after_redirect - t_after_reset) : 0ULL;
        uint64_t read_ns = (t_after_read >= t_after_redirect) ? (t_after_read - t_after_redirect) : 0ULL;
        uint64_t capture_ns = (t_after_capture >= t_after_read) ? (t_after_capture - t_after_read) : 0ULL;
        uint64_t restore_ns = (t_after_restore >= t_after_capture) ? (t_after_restore - t_after_capture) : 0ULL;
        fprintf(stderr,
                "[insight-perf] api=%s ret=%d errno=%d total=%.3fms config=%.3fms reset=%.3fms redirect=%.3fms read=%.3fms capture=%.3fms restore=%.3fms\n",
                insight_query_type_name(query_type),
                ret,
                (ret == 0) ? 0 : (saved_errno == 0 ? EIO : saved_errno),
                (double)total_ns / 1000000.0,
                (double)config_ns / 1000000.0,
                (double)reset_ns / 1000000.0,
                (double)redirect_ns / 1000000.0,
                (double)read_ns / 1000000.0,
                (double)capture_ns / 1000000.0,
                (double)restore_ns / 1000000.0);
        fprintf(stderr,
                "[insight-perf] nvme-read pipeline=%.3fms io=%.3fms(post-calls=%" PRIu64 ") post-action=%.3fms(io-calls=%" PRIu64 ")\n",
                (double)nvme_perf.pipeline_total_ns / 1000000.0,
                (double)nvme_perf.io_ns / 1000000.0,
                nvme_perf.post_action_calls,
                (double)nvme_perf.post_action_ns / 1000000.0,
                nvme_perf.io_calls);
    }
#endif
    return ret;
}

int get_read_latency_percentiles(const char *device,
                                 const char *time_start,
                                 const char *time_end,
                                 uint64_t lba_start,
                                 uint64_t lba_end,
                                 char *json_buffer) {
    return extract_latency_bucket_result(device, time_start, time_end,
                                         lba_start, lba_end,
                                         "get_read_latency_percentiles",
                                         "read",
                                         json_buffer);
}

int get_write_latency_percentiles(const char *device,
                                  const char *time_start,
                                  const char *time_end,
                                  uint64_t lba_start,
                                  uint64_t lba_end,
                                  char *json_buffer) {
    return extract_latency_bucket_result(device, time_start, time_end,
                                         lba_start, lba_end,
                                         "get_write_latency_percentiles",
                                         "write",
                                         json_buffer);
}

int get_write_amplification(const char *device,
                            const char *time_start,
                            const char *time_end,
                            uint64_t lba_start,
                            uint64_t lba_end,
                            char *json_buffer) {
    return extract_write_amplification_from_samples(device, 0ULL,
                                                    time_start, time_end,
                                                    lba_start, lba_end,
                                                    json_buffer);
}

int get_qd_distribution(const char *device,
                        const char *time_start,
                        const char *time_end,
                        uint64_t lba_start,
                        uint64_t lba_end,
                        char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, 0ULL, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_QD,
                                           "get_qd_distribution",
                                           json_buffer);
}

int get_read_size_distribution(const char *device,
                               const char *time_start,
                               const char *time_end,
                               uint64_t lba_start,
                               uint64_t lba_end,
                               char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, 0ULL, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_READ_SIZE,
                                           "get_read_size_distribution",
                                           json_buffer);
}

int get_write_size_distribution(const char *device,
                                const char *time_start,
                                const char *time_end,
                                uint64_t lba_start,
                                uint64_t lba_end,
                                char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, 0ULL, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_WRITE_SIZE,
                                           "get_write_size_distribution",
                                           json_buffer);
}

int get_read_throughput_distribution(const char *device,
                                     const char *time_start,
                                     const char *time_end,
                                     uint64_t lba_start,
                                     uint64_t lba_end,
                                     char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, 0ULL, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_READ_THROUGHPUT,
                                           "get_read_throughput_distribution",
                                           json_buffer);
}

int get_write_throughput_distribution(const char *device,
                                      const char *time_start,
                                      const char *time_end,
                                      uint64_t lba_start,
                                      uint64_t lba_end,
                                      char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, 0, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_WRITE_THROUGHPUT,
                                           "get_write_throughput_distribution",
                                           json_buffer);
}

int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                uint64_t lba_start,
                                uint64_t lba_end,
                                char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, block_size, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_READ_COUNT,
                                           "get_read_count_distribution",
                                           json_buffer);
}

int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         uint64_t lba_start,
                                         uint64_t lba_end,
                                         char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, block_size, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_W2FR,
                                           "get_write_to_first_read_distribution",
                                           json_buffer);
}

int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               uint64_t lba_start,
                               uint64_t lba_end,
                               char *json_buffer) {
    return run_query_and_fill_wrapped_json(device, block_size, time_start, time_end,
                                           lba_start, lba_end,
                                           INSIGHT_QUERY_LIFECYCLE,
                                           "get_lifecycle_distribution",
                                           json_buffer);
}

int get_nand_write_volume(const char *device,
                          const char *time_start,
                          const char *time_end,
                          uint64_t lba_start,
                          uint64_t lba_end,
                          char *json_buffer) {
    return extract_stat_volume_from_samples(device,
                                            NVME_LBA_SIZE_BYTES,
                                            time_start,
                                            time_end,
                                            lba_start,
                                            lba_end,
                                            INSIGHT_QUERY_NAND_WRITE_VOLUME,
                                            json_buffer);
}

int get_gc_data_movement(const char *device,
                         const char *time_start,
                         const char *time_end,
                         uint64_t lba_start,
                         uint64_t lba_end,
                         char *json_buffer) {
    return extract_stat_volume_from_samples(device,
                                            NVME_LBA_SIZE_BYTES,
                                            time_start,
                                            time_end,
                                            lba_start,
                                            lba_end,
                                            INSIGHT_QUERY_GC_DATA_MOVEMENT,
                                            json_buffer);
}
