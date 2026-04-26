#include "insight_api.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int expect_invalid(const char *name, int rc) {
    if (rc == 0 || errno != EINVAL) {
        fprintf(stderr, "%s expected EINVAL, got rc=%d errno=%d (%s)\n",
                name, rc, errno, strerror(errno));
        return -1;
    }
    return 0;
}

int main(void) {
    char json_buf[INSIGHT_JSON_BUFFER_BYTES];
    int rc = 0;

    errno = 0;
    rc = get_read_latency_percentiles(NULL, 4096ULL,
                                      "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                      json_buf);
    if (expect_invalid("get_read_latency_percentiles(NULL device)", rc) != 0) {
        return 1;
    }

    errno = 0;
    rc = get_write_amplification("/dev/null", 0ULL,
                                 "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                 json_buf);
    if (expect_invalid("get_write_amplification(zero block_size)", rc) != 0) {
        return 1;
    }

    errno = 0;
    rc = get_qd_distribution("/dev/null", 4096ULL,
                             "bad-time", "2024-03-09 16:00:15",
                             json_buf);
    if (expect_invalid("get_qd_distribution(invalid time_start)", rc) != 0) {
        return 1;
    }

    errno = 0;
    rc = get_read_size_distribution("/dev/null", 0ULL,
                                    "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                    json_buf);
    if (expect_invalid("get_read_size_distribution(zero block_size)", rc) != 0) {
        return 1;
    }

    errno = 0;
    rc = get_write_size_distribution(NULL, 4096ULL,
                                     "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                     json_buf);
    if (expect_invalid("get_write_size_distribution(NULL device)", rc) != 0) {
        return 1;
    }

    errno = 0;
    rc = get_read_throughput_distribution(NULL, 4096ULL,
                                          "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                          json_buf);
    if (expect_invalid("get_read_throughput_distribution(NULL device)", rc) != 0) {
        return 1;
    }

    errno = 0;
    rc = get_write_throughput_distribution("/dev/null", 0ULL,
                                           "2024-03-09 16:00:05", "2024-03-09 16:00:15",
                                           json_buf);
    if (expect_invalid("get_write_throughput_distribution(zero block_size)", rc) != 0) {
        return 1;
    }

    return 0;
}
