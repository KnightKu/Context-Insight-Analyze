#include "nvme_read.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u64_with_unit(const char *arg, uint64_t *out_value) {
    if (arg == NULL || out_value == NULL || *arg == '\0') {
        return -1;
    }

    char *endptr = NULL;
    errno = 0;
    unsigned long long base = strtoull(arg, &endptr, 10);
    if (errno != 0 || endptr == arg) {
        return -1;
    }

    // Accept raw number or optional binary suffix: K/M/G/T (and optional trailing B/b).
    uint64_t multiplier = 1ULL;
    if (*endptr != '\0') {
        char unit = (char)toupper((unsigned char)*endptr);
        switch (unit) {
            case 'K':
                multiplier = 1024ULL;
                break;
            case 'M':
                multiplier = 1024ULL * 1024ULL;
                break;
            case 'G':
                multiplier = 1024ULL * 1024ULL * 1024ULL;
                break;
            case 'T':
                multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
                break;
            default:
                return -1;
        }

        ++endptr;
        if (*endptr == 'B' || *endptr == 'b') {
            ++endptr;
        }
        if (*endptr != '\0') {
            return -1;
        }
    }

    // Guard against multiplication overflow before converting to final value.
    if (base > (ULLONG_MAX / multiplier)) {
        errno = ERANGE;
        return -1;
    }

    *out_value = (uint64_t)base * multiplier;
    return 0;
}

int main(int argc, char *argv[]) {
    int argi = 1;
    int debug_enabled = 0;
    int latency_enabled = 0;
    const char *bucket_csv_path = NULL;
    uint64_t bucket_csv_start = 0ULL;
    uint64_t bucket_csv_count = 0ULL;
    const char *advanced_csv_path = NULL;
    const char *stat_qd_csv_path = NULL;
    const char *stat_wa_csv_path = NULL;
    while (argi < argc) {
        if (strcmp(argv[argi], "-D") == 0 || strcmp(argv[argi], "--debug") == 0) {
            debug_enabled = 1;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "-l") == 0 || strcmp(argv[argi], "--latency") == 0) {
            latency_enabled = 1;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "--export-bucket-csv") == 0) {
            if ((argi + 3) >= argc) {
                fprintf(stderr, "missing args for --export-bucket-csv <path> <start_bucket> <bucket_count>\n");
                return 1;
            }
            bucket_csv_path = argv[argi + 1];
            if (parse_u64_with_unit(argv[argi + 2], &bucket_csv_start) != 0) {
                fprintf(stderr, "invalid bucket csv start_bucket: %s\n", argv[argi + 2]);
                return 1;
            }
            if (parse_u64_with_unit(argv[argi + 3], &bucket_csv_count) != 0 || bucket_csv_count == 0ULL) {
                fprintf(stderr, "invalid bucket csv bucket_count: %s\n", argv[argi + 3]);
                return 1;
            }
            argi += 4;
            continue;
        }
        if (strcmp(argv[argi], "--export-advanced-csv") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "missing args for --export-advanced-csv <path>\n");
                return 1;
            }
            advanced_csv_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strcmp(argv[argi], "--export-stat-qd-csv") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "missing args for --export-stat-qd-csv <path>\n");
                return 1;
            }
            stat_qd_csv_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strcmp(argv[argi], "--export-stat-wa-csv") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "missing args for --export-stat-wa-csv <path>\n");
                return 1;
            }
            stat_wa_csv_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

    if ((argc - argi) != 3) {
        fprintf(stderr,
                "usage: %s [-D|--debug] [-l|--latency] "
                "[--export-bucket-csv <path> <start_bucket> <bucket_count>] "
                "[--export-advanced-csv <path>] "
                "[--export-stat-qd-csv <path>] "
                "[--export-stat-wa-csv <path>] "
                "<device_name> <slba[K|M|G|T]> <data_len[K|M|G|T]>\n",
                argv[0]);
        return 1;
    }

    const char *device_name = argv[argi];
    // slba and data_len share the same unit parser for consistent CLI behavior.
    uint64_t slba = 0ULL;
    if (parse_u64_with_unit(argv[argi + 1], &slba) != 0) {
        fprintf(stderr, "invalid slba: %s\n", argv[argi + 1]);
        return 1;
    }

    uint64_t data_len = 0;
    if (parse_u64_with_unit(argv[argi + 2], &data_len) != 0 || data_len == 0ULL) {
        fprintf(stderr, "invalid data_len: %s (examples: 128K, 64M, 1G, 1T)\n", argv[argi + 2]);
        return 1;
    }

    nvme_read_set_debug(debug_enabled);
    nvme_read_set_latency(latency_enabled);

    if (nvme_read(device_name, slba, data_len, NULL) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    if (bucket_csv_path != NULL) {
        if (nvme_post_action_export_stats_csv(bucket_csv_path, bucket_csv_start, bucket_csv_count) != 0) {
            fprintf(stderr, "bucket csv export failed: %s\n", strerror(errno));
            return 1;
        }
        fprintf(stderr,
                "bucket csv export success: path=%s start=%" PRIu64 " count=%" PRIu64 "\n",
                bucket_csv_path, bucket_csv_start, bucket_csv_count);
    }
    if (advanced_csv_path != NULL) {
        if (nvme_post_action_export_advanced_life_cycle_csv(advanced_csv_path) != 0) {
            fprintf(stderr, "advanced csv export failed: %s\n", strerror(errno));
            return 1;
        }
        fprintf(stderr, "advanced csv export success: path=%s\n", advanced_csv_path);
    }
    if (stat_qd_csv_path != NULL) {
        if (nvme_post_action_export_stat_qd_csv(stat_qd_csv_path) != 0) {
            fprintf(stderr, "stat qd csv export failed: %s\n", strerror(errno));
            return 1;
        }
        fprintf(stderr, "stat qd csv export success: path=%s\n", stat_qd_csv_path);
    }
    if (stat_wa_csv_path != NULL) {
        if (nvme_post_action_export_stat_wa_csv(stat_wa_csv_path) != 0) {
            fprintf(stderr, "stat wa csv export failed: %s\n", strerror(errno));
            return 1;
        }
        fprintf(stderr, "stat wa csv export success: path=%s\n", stat_wa_csv_path);
    }

    fprintf(stderr, "nvme passthru read done. slba=%" PRIu64 " data_len=%" PRIu64 "\n",
            slba, data_len);
    return 0;
}
