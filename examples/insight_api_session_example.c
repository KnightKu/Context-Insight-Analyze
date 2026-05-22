#include "insight_api.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define INSIGHT_API_RUN(section, call, exit_code)                          \
    do {                                                                     \
        if ((call) != 0) {                                                   \
            if (errno == ENODATA)                                            \
                break;                                                       \
            fprintf(stderr, #call " failed: %s\n", strerror(errno));         \
            return (exit_code);                                              \
        }                                                                    \
        printf("=== " section " ===\n%s\n\n", json_buffer);                \
    } while (0)

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

    printf("session_id=%" PRIu64 " block_size=%" PRIu64
           " (time/LBA from insight_metalog_read; time_start/time_end args ignored)\n\n",
           session_id, block_size);

    char json_buffer[INSIGHT_JSON_BUFFER_BYTES];
    const int64_t sid = (int64_t)session_id;
    /* Caller time strings are ignored when session_id >= 0; placeholders only. */
    const char *ignored_time = "1970-01-01 00:00:00";

    INSIGHT_API_RUN("read_latency_percentiles",
                    get_read_latency_percentiles(device, ignored_time, ignored_time, sid,
                                                 json_buffer),
                    4);
    INSIGHT_API_RUN("write_latency_percentiles",
                    get_write_latency_percentiles(device, ignored_time, ignored_time, sid,
                                                  json_buffer),
                    14);
    INSIGHT_API_RUN("write_amplification",
                    get_write_amplification(device, ignored_time, ignored_time, sid, json_buffer),
                    5);
    INSIGHT_API_RUN("qd_distribution",
                    get_qd_distribution(device, ignored_time, ignored_time, sid, json_buffer),
                    6);
    INSIGHT_API_RUN("read_size_distribution",
                    get_read_size_distribution(device, ignored_time, ignored_time, sid,
                                               json_buffer),
                    7);
    INSIGHT_API_RUN("write_size_distribution",
                    get_write_size_distribution(device, ignored_time, ignored_time, sid,
                                                json_buffer),
                    8);
    INSIGHT_API_RUN("read_throughput_distribution",
                    get_read_throughput_distribution(device, ignored_time, ignored_time, sid,
                                                     json_buffer),
                    9);
    INSIGHT_API_RUN("write_throughput_distribution",
                    get_write_throughput_distribution(device, ignored_time, ignored_time, sid,
                                                      json_buffer),
                    10);
    INSIGHT_API_RUN("read_count_distribution",
                    get_read_count_distribution(device, block_size, ignored_time, ignored_time,
                                                sid, json_buffer),
                    11);
    INSIGHT_API_RUN("write_to_first_read_distribution",
                    get_write_to_first_read_distribution(device, block_size, ignored_time,
                                                         ignored_time, sid, json_buffer),
                    12);
    INSIGHT_API_RUN("lifecycle_distribution",
                    get_lifecycle_distribution(device, block_size, ignored_time, ignored_time,
                                               sid, json_buffer),
                    13);
    INSIGHT_API_RUN("nand_write_volume",
                    get_nand_write_volume(device, ignored_time, ignored_time, sid, json_buffer),
                    15);
    INSIGHT_API_RUN("gc_data_movement",
                    get_gc_data_movement(device, ignored_time, ignored_time, sid, json_buffer),
                    16);

    return 0;
}
