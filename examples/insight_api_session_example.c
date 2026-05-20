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

    const int64_t query_session_id = (int64_t)session_id;
    printf("session_id=%" PRIu64 " block_size=%" PRIu64
           " (time/LBA resolved inside insight_api via metalog)\n\n",
           (uint64_t)session_id, block_size);

    char json_buffer[INSIGHT_JSON_BUFFER_BYTES];

    if (get_read_latency_percentiles(device, NULL, NULL, 0ULL, 0ULL, query_session_id,
                                     json_buffer) != 0) {
        fprintf(stderr, "get_read_latency_percentiles failed: %s\n", strerror(errno));
        return 4;
    }
    printf("=== read_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_latency_percentiles(device, NULL, NULL, 0ULL, 0ULL, query_session_id,
                                      json_buffer) != 0) {
        fprintf(stderr, "get_write_latency_percentiles failed: %s\n", strerror(errno));
        return 14;
    }
    printf("=== write_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_amplification(device, NULL, NULL, 0ULL, 0ULL, query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_write_amplification failed: %s\n", strerror(errno));
        return 5;
    }
    printf("=== write_amplification ===\n%s\n\n", json_buffer);

    if (get_qd_distribution(device, NULL, NULL, 0ULL, 0ULL, query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_qd_distribution failed: %s\n", strerror(errno));
        return 6;
    }
    printf("=== qd_distribution ===\n%s\n\n", json_buffer);

    if (get_read_size_distribution(device, NULL, NULL, 0ULL, 0ULL, query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_read_size_distribution failed: %s\n", strerror(errno));
        return 7;
    }
    printf("=== read_size_distribution ===\n%s\n\n", json_buffer);

    if (get_write_size_distribution(device, NULL, NULL, 0ULL, 0ULL, query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_write_size_distribution failed: %s\n", strerror(errno));
        return 8;
    }
    printf("=== write_size_distribution ===\n%s\n\n", json_buffer);

    if (get_read_throughput_distribution(device, NULL, NULL, 0ULL, 0ULL, query_session_id,
                                         json_buffer) != 0) {
        fprintf(stderr, "get_read_throughput_distribution failed: %s\n", strerror(errno));
        return 9;
    }
    printf("=== read_throughput_distribution ===\n%s\n\n", json_buffer);

    if (get_write_throughput_distribution(device, NULL, NULL, 0ULL, 0ULL, query_session_id,
                                          json_buffer) != 0) {
        fprintf(stderr, "get_write_throughput_distribution failed: %s\n", strerror(errno));
        return 10;
    }
    printf("=== write_throughput_distribution ===\n%s\n", json_buffer);

    if (get_read_count_distribution(device, block_size, NULL, NULL, 0ULL, 0ULL, query_session_id,
                                    json_buffer) != 0) {
        fprintf(stderr, "get_read_count_distribution failed: %s\n", strerror(errno));
        return 11;
    }
    printf("=== read_count_distribution ===\n%s\n\n", json_buffer);

    if (get_write_to_first_read_distribution(device, block_size, NULL, NULL, 0ULL, 0ULL,
                                             query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_write_to_first_read_distribution failed: %s\n", strerror(errno));
        return 12;
    }
    printf("=== write_to_first_read_distribution ===\n%s\n\n", json_buffer);

    if (get_lifecycle_distribution(device, block_size, NULL, NULL, 0ULL, 0ULL, query_session_id,
                                   json_buffer) != 0) {
        fprintf(stderr, "get_lifecycle_distribution failed: %s\n", strerror(errno));
        return 13;
    }
    printf("=== lifecycle_distribution ===\n%s\n\n", json_buffer);

    if (get_nand_write_volume(device, NULL, NULL, 0ULL, 0ULL, query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_nand_write_volume failed: %s\n", strerror(errno));
        return 15;
    }
    printf("=== nand_write_volume ===\n%s\n\n", json_buffer);

    if (get_gc_data_movement(device, NULL, NULL, 0ULL, 0ULL, query_session_id, json_buffer) != 0) {
        fprintf(stderr, "get_gc_data_movement failed: %s\n", strerror(errno));
        return 16;
    }
    printf("=== gc_data_movement ===\n%s\n", json_buffer);

    return 0;
}
