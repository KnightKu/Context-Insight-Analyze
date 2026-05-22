#include "insight_api.h"

#include <errno.h>
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

/*
 * Run an insight_api get_* call: print JSON on success; on ENODATA skip output
 * and continue; on other errors print to stderr and return exit_code from main.
 */
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
    if (argc != 4 && argc != 5) {
        fprintf(stderr,
                "usage: %s <device> <time_start \"YYYY-MM-DD HH:MM:SS\"> "
                "<time_end \"YYYY-MM-DD HH:MM:SS\"> [block_size_bytes]\n",
                argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const char *time_start = argv[2];
    const char *time_end = argv[3];
    uint64_t block_size = 4096ULL;
    if (argc == 5) {
        if (parse_u64_str(argv[4], &block_size) != 0 || block_size == 0ULL) {
            fprintf(stderr, "invalid block_size_bytes: %s\n", argv[4]);
            return 1;
        }
    }

    char json_buffer[INSIGHT_JSON_BUFFER_BYTES];

    INSIGHT_API_RUN("read_latency_percentiles",
                    get_read_latency_percentiles(device, time_start, time_end,
                                                 INSIGHT_JSON_QUERY_SESSION_ID_NONE,
                                                 json_buffer),
                    2);
    INSIGHT_API_RUN("write_latency_percentiles",
                    get_write_latency_percentiles(device, time_start, time_end,
                                                INSIGHT_JSON_QUERY_SESSION_ID_NONE,
                                                json_buffer),
                    14);
    INSIGHT_API_RUN("write_amplification",
                    get_write_amplification(device, time_start, time_end,
                                            INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    3);
    INSIGHT_API_RUN("qd_distribution",
                    get_qd_distribution(device, time_start, time_end,
                                        INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    4);
    INSIGHT_API_RUN("read_size_distribution",
                    get_read_size_distribution(device, time_start, time_end,
                                               INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    5);
    INSIGHT_API_RUN("write_size_distribution",
                    get_write_size_distribution(device, time_start, time_end,
                                                INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    6);
    INSIGHT_API_RUN("read_throughput_distribution",
                    get_read_throughput_distribution(device, time_start, time_end,
                                                     INSIGHT_JSON_QUERY_SESSION_ID_NONE,
                                                     json_buffer),
                    7);
    INSIGHT_API_RUN("write_throughput_distribution",
                    get_write_throughput_distribution(device, time_start, time_end,
                                                      INSIGHT_JSON_QUERY_SESSION_ID_NONE,
                                                      json_buffer),
                    8);
    INSIGHT_API_RUN("read_count_distribution",
                    get_read_count_distribution(device, block_size, time_start, time_end,
                                                INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    9);
    INSIGHT_API_RUN("write_to_first_read_distribution",
                    get_write_to_first_read_distribution(device, block_size, time_start,
                                                         time_end,
                                                         INSIGHT_JSON_QUERY_SESSION_ID_NONE,
                                                         json_buffer),
                    10);
    INSIGHT_API_RUN("lifecycle_distribution",
                    get_lifecycle_distribution(device, block_size, time_start, time_end,
                                               INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    11);
    INSIGHT_API_RUN("nand_write_volume",
                    get_nand_write_volume(device, time_start, time_end,
                                          INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    12);
    INSIGHT_API_RUN("gc_data_movement",
                    get_gc_data_movement(device, time_start, time_end,
                                         INSIGHT_JSON_QUERY_SESSION_ID_NONE, json_buffer),
                    13);

    return 0;
}
