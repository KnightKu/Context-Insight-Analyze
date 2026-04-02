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
                return 2;
            }
            bucket_csv_path = argv[argi + 1];
            if (parse_u64_with_unit(argv[argi + 2], &bucket_csv_start) != 0) {
                fprintf(stderr, "invalid bucket csv start_bucket: %s\n", argv[argi + 2]);
                return 2;
            }
            if (parse_u64_with_unit(argv[argi + 3], &bucket_csv_count) != 0 || bucket_csv_count == 0ULL) {
                fprintf(stderr, "invalid bucket csv bucket_count: %s\n", argv[argi + 3]);
                return 2;
            }
            argi += 4;
            continue;
        }
        if (strcmp(argv[argi], "--export-advanced-csv") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "missing args for --export-advanced-csv <path>\n");
                return 2;
            }
            advanced_csv_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strcmp(argv[argi], "--export-stat-qd-csv") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "missing args for --export-stat-qd-csv <path>\n");
                return 2;
            }
            stat_qd_csv_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strcmp(argv[argi], "--export-stat-wa-csv") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "missing args for --export-stat-wa-csv <path>\n");
                return 2;
            }
            stat_wa_csv_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

    if ((argc - argi) != 1 && (argc - argi) != 2) {
        fprintf(stderr,
                "usage: %s [-D|--debug] [-l|--latency] "
                "[--export-bucket-csv <path> <start_bucket> <bucket_count>] "
                "[--export-advanced-csv <path>] "
                "[--export-stat-qd-csv <path>] "
                "[--export-stat-wa-csv <path>] "
                "<input_file> [offset_bytes]\n",
                argv[0]);
        return 2;
    }

    uint64_t offset_bytes = 0ULL;
    if ((argc - argi) == 2) {
        if (parse_u64_with_unit(argv[argi + 1], &offset_bytes) != 0) {
            fprintf(stderr, "invalid offset_bytes: %s\n", argv[argi + 1]);
            return 2;
        }
    }

    const char *input_file = argv[argi];
    FILE *fp = fopen(input_file, "rb");
    if (fp == NULL) {
        fprintf(stderr, "open %s failed: %s\n", input_file, strerror(errno));
        return 2;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fprintf(stderr, "fseek end failed: %s\n", strerror(errno));
        fclose(fp);
        return 2;
    }
    long file_size = ftell(fp);
    if (file_size < 0) {
        fprintf(stderr, "ftell failed: %s\n", strerror(errno));
        fclose(fp);
        return 2;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "fseek set failed: %s\n", strerror(errno));
        fclose(fp);
        return 2;
    }

    uint8_t *buf = NULL;
    size_t len = (size_t)file_size;
    if (len > 0U) {
        buf = (uint8_t *)malloc(len);
        if (buf == NULL) {
            fprintf(stderr, "malloc failed for %zu bytes\n", len);
            fclose(fp);
            return 2;
        }
        size_t got = fread(buf, 1U, len, fp);
        if (got != len) {
            fprintf(stderr, "fread failed: expect=%zu got=%zu\n", len, got);
            free(buf);
            fclose(fp);
            return 2;
        }
    }
    fclose(fp);

    if (len > (size_t)UINT32_MAX) {
        fprintf(stderr, "file too large for post_action: %zu bytes\n", len);
        free(buf);
        return 2;
    }

    nvme_read_set_debug(debug_enabled);
    nvme_read_set_latency(latency_enabled);
    int rc = nvme_post_action_process(buf, (uint32_t)len, offset_bytes);
    if (rc != 0) {
        if (errno == ECANCELED) {
            fprintf(stderr,
                    "post action soft-stop: overwrite detected, stop further parse at offset=%" PRIu64 "\n",
                    offset_bytes);
        } else {
            fprintf(stderr, "post action failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
    }

    if (bucket_csv_path != NULL) {
        if (nvme_post_action_export_stats_csv(bucket_csv_path, bucket_csv_start, bucket_csv_count) != 0) {
            fprintf(stderr, "export csv failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
        fprintf(stderr,
                "csv export success: path=%s start_bucket=%" PRIu64 " bucket_count=%" PRIu64 "\n",
                bucket_csv_path, bucket_csv_start, bucket_csv_count);
    }
    if (advanced_csv_path != NULL) {
        if (nvme_post_action_export_advanced_life_cycle_csv(advanced_csv_path) != 0) {
            fprintf(stderr, "advanced csv export failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
        fprintf(stderr, "advanced csv export success: path=%s\n", advanced_csv_path);
    }
    if (stat_qd_csv_path != NULL) {
        if (nvme_post_action_export_stat_qd_csv(stat_qd_csv_path) != 0) {
            fprintf(stderr, "export stat qd csv failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
        fprintf(stderr, "stat qd csv export success: path=%s\n", stat_qd_csv_path);
    }
    if (stat_wa_csv_path != NULL) {
        if (nvme_post_action_export_stat_wa_csv(stat_wa_csv_path) != 0) {
            fprintf(stderr, "export stat wa csv failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
        fprintf(stderr, "stat wa csv export success: path=%s\n", stat_wa_csv_path);
    }

    fprintf(stderr, "post action success: file=%s bytes=%zu offset=%" PRIu64 "\n",
            input_file, len, offset_bytes);
    free(buf);
    return 0;
}
