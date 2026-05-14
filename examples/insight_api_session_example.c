#include "insight_api.h"
#include "insight_metalog.h"
#include "nvme_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Match insight_api.c scan span for nvme_read family. */
#define SESSION_EXAMPLE_SCAN_SLBA_BYTES 0ULL
#define SESSION_EXAMPLE_SCAN_DATA_LEN_11T_BYTES (11ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)

static uint64_t session_example_scan_bytes(void) {
    uint64_t log_span_bytes = 0ULL;
    if (LOG_END_LBA > LOG_START_LBA) {
        log_span_bytes = LOG_END_LBA - LOG_START_LBA;
    }
    if (log_span_bytes == 0ULL) {
        return 0ULL;
    }
    return (SESSION_EXAMPLE_SCAN_DATA_LEN_11T_BYTES < log_span_bytes) ?
        SESSION_EXAMPLE_SCAN_DATA_LEN_11T_BYTES : log_span_bytes;
}

static int parse_u64_str(const char *s, uint64_t *out) {
    if (s == NULL || out == NULL || *s == '\0') {
        return -1;
    }
    char *endptr = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &endptr, 10);
    if (errno != 0 || endptr == s || *endptr != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

/**
 * insight_api JSON uses a fixed "query" layout with time_start / time_end.
 * For this example, replace those keys with numeric session_id only.
 */
static int session_example_rewrite_query_json(char *buf, size_t buf_size, uint64_t session_id) {
    if (buf == NULL || buf_size == 0U) {
        return -1;
    }
    const char *ts = strstr(buf, "    \"time_start\":");
    if (ts == NULL) {
        return -1;
    }
    const char *te_key = strstr(ts, "    \"time_end\":");
    if (te_key == NULL) {
        return -1;
    }
    const char *colon = strchr(te_key + 1, ':');
    if (colon == NULL) {
        return -1;
    }
    const char *q = colon + 1;
    while (*q == ' ' || *q == '\t') {
        ++q;
    }
    if (*q != '"') {
        return -1;
    }
    ++q;
    while (*q != '\0') {
        if (*q == '\\' && q[1] != '\0') {
            q += 2;
            continue;
        }
        if (*q == '"') {
            break;
        }
        ++q;
    }
    if (*q != '"') {
        return -1;
    }
    ++q;
    if (*q == '\r') {
        ++q;
    }
    if (*q == '\n') {
        ++q;
    }

    char insert[96];
    int ilen = snprintf(insert, sizeof(insert), "    \"session_id\": %" PRIu64 "\n", session_id);
    if (ilen < 0 || (size_t)ilen >= sizeof(insert)) {
        return -1;
    }

    size_t prefix_len = (size_t)(ts - (const char *)buf);
    size_t suffix_len = strlen(q);
    if (prefix_len + (size_t)ilen + suffix_len + 1U > buf_size) {
        return -1;
    }

    char tmp[INSIGHT_JSON_BUFFER_BYTES];
    memcpy(tmp, buf, prefix_len);
    memcpy(tmp + prefix_len, insert, (size_t)ilen);
    memcpy(tmp + prefix_len + (size_t)ilen, q, suffix_len + 1U);
    memcpy(buf, tmp, prefix_len + (size_t)ilen + suffix_len + 1U);
    return 0;
}

static int rewrite_session_json_or_fail(char *json_buffer, uint64_t session_id) {
    if (session_example_rewrite_query_json(json_buffer, INSIGHT_JSON_BUFFER_BYTES, session_id) != 0) {
        fprintf(stderr, "session_example_rewrite_query_json failed\n");
        return 17;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <device> <session_id> <block_size_bytes>\n", argv[0]);
        return 1;
    }

    const char *device = argv[1];
    uint64_t session_id = 0ULL;
    if (parse_u64_str(argv[2], &session_id) != 0) {
        fprintf(stderr, "invalid session_id: %s\n", argv[2]);
        return 1;
    }

    uint64_t block_size = 0ULL;
    if (parse_u64_str(argv[3], &block_size) != 0 || block_size == 0ULL) {
        fprintf(stderr, "invalid block_size_bytes: %s\n", argv[3]);
        return 1;
    }

    uint64_t scan_bytes = session_example_scan_bytes();
    if (scan_bytes == 0ULL) {
        fprintf(stderr, "invalid log span (LOG_START_LBA / LOG_END_LBA)\n");
        return 1;
    }

    struct insight_metalog_session_summary *sessions = NULL;
    size_t session_count = 0U;
    if (insight_metalog_read(device, &sessions, &session_count) != 0) {
        fprintf(stderr, "insight_metalog_read failed: %s\n", strerror(errno));
        return 2;
    }

    char time_start[INSIGHT_METALOG_TIME_STR_BUFSIZ];
    char time_end[INSIGHT_METALOG_TIME_STR_BUFSIZ];
    uint64_t lba_byte_offset_start = 0ULL;
    uint64_t lba_byte_offset_end = 0ULL;
    if (insight_get_session_time_window(sessions, session_count, session_id, time_start,
                                        sizeof(time_start), time_end, sizeof(time_end),
                                        &lba_byte_offset_start, &lba_byte_offset_end) != 0) {
        fprintf(stderr, "insight_get_session_time_window failed: %s\n", strerror(errno));
        insight_metalog_sessions_free(sessions);
        return 3;
    }

    insight_metalog_sessions_free(sessions);
    sessions = NULL;

    uint64_t byte_lo = lba_byte_offset_start;
    uint64_t byte_hi = lba_byte_offset_end;
    if (byte_lo > byte_hi) {
        uint64_t t = byte_lo;
        byte_lo = byte_hi;
        byte_hi = t;
    }
    const uint64_t lba_filter_start = byte_lo / NVME_LBA_SIZE_BYTES;
    const uint64_t lba_filter_end = byte_hi / NVME_LBA_SIZE_BYTES;

    printf("session_id=%" PRIu64 " time_start=\"%s\" time_end=\"%s\" "
           "lba_byte_offset_start=%" PRIu64 " lba_byte_offset_end=%" PRIu64 " "
           "insight_lba_start=%" PRIu64 " insight_lba_end=%" PRIu64 "\n\n",
           (uint64_t)session_id, time_start, time_end, lba_byte_offset_start, lba_byte_offset_end,
           lba_filter_start, lba_filter_end);

    char json_buffer[INSIGHT_JSON_BUFFER_BYTES];

    if (get_read_latency_percentiles(device, time_start, time_end, lba_filter_start, lba_filter_end,
                                     json_buffer) != 0) {
        fprintf(stderr, "get_read_latency_percentiles failed: %s\n", strerror(errno));
        return 4;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== read_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_latency_percentiles(device, time_start, time_end, lba_filter_start, lba_filter_end,
                                      json_buffer) != 0) {
        fprintf(stderr, "get_write_latency_percentiles failed: %s\n", strerror(errno));
        return 14;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== write_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_amplification(device, time_start, time_end, lba_filter_start, lba_filter_end,
                                json_buffer) != 0) {
        fprintf(stderr, "get_write_amplification failed: %s\n", strerror(errno));
        return 5;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== write_amplification ===\n%s\n\n", json_buffer);

    if (get_qd_distribution(device, time_start, time_end, lba_filter_start, lba_filter_end,
                            json_buffer) != 0) {
        fprintf(stderr, "get_qd_distribution failed: %s\n", strerror(errno));
        return 6;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== qd_distribution ===\n%s\n\n", json_buffer);

    if (get_read_size_distribution(device, time_start, time_end, lba_filter_start, lba_filter_end,
                                  json_buffer) != 0) {
        fprintf(stderr, "get_read_size_distribution failed: %s\n", strerror(errno));
        return 7;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== read_size_distribution ===\n%s\n\n", json_buffer);

    if (get_write_size_distribution(device, time_start, time_end, lba_filter_start, lba_filter_end,
                                   json_buffer) != 0) {
        fprintf(stderr, "get_write_size_distribution failed: %s\n", strerror(errno));
        return 8;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== write_size_distribution ===\n%s\n\n", json_buffer);

    if (get_read_throughput_distribution(device, time_start, time_end, lba_filter_start,
                                       lba_filter_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_throughput_distribution failed: %s\n", strerror(errno));
        return 9;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== read_throughput_distribution ===\n%s\n\n", json_buffer);

    if (get_write_throughput_distribution(device, time_start, time_end, lba_filter_start,
                                         lba_filter_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_throughput_distribution failed: %s\n", strerror(errno));
        return 10;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== write_throughput_distribution ===\n%s\n", json_buffer);

    if (get_read_count_distribution(device, block_size, time_start, time_end, lba_filter_start,
                                    lba_filter_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_count_distribution failed: %s\n", strerror(errno));
        return 11;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== read_count_distribution ===\n%s\n\n", json_buffer);

    if (get_write_to_first_read_distribution(device, block_size, time_start, time_end,
                                             lba_filter_start, lba_filter_end, json_buffer) !=
        0) {
        fprintf(stderr, "get_write_to_first_read_distribution failed: %s\n", strerror(errno));
        return 12;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== write_to_first_read_distribution ===\n%s\n\n", json_buffer);

    if (get_lifecycle_distribution(device, block_size, time_start, time_end, lba_filter_start,
                                    lba_filter_end, json_buffer) !=
        0) {
        fprintf(stderr, "get_lifecycle_distribution failed: %s\n", strerror(errno));
        return 13;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== lifecycle_distribution ===\n%s\n\n", json_buffer);

    if (get_nand_write_volume(device, time_start, time_end, lba_filter_start, lba_filter_end,
                              json_buffer) != 0) {
        fprintf(stderr, "get_nand_write_volume failed: %s\n", strerror(errno));
        return 15;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== nand_write_volume ===\n%s\n\n", json_buffer);

    if (get_gc_data_movement(device, time_start, time_end, lba_filter_start, lba_filter_end,
                             json_buffer) != 0) {
        fprintf(stderr, "get_gc_data_movement failed: %s\n", strerror(errno));
        return 16;
    }
    if (rewrite_session_json_or_fail(json_buffer, session_id) != 0) {
        return 17;
    }
    printf("=== gc_data_movement ===\n%s\n", json_buffer);

    return 0;
}
