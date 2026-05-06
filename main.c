#include "nvme_read.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int parse_time_window_ms(const char *arg, uint64_t *out_ms) {
    if (arg == NULL || out_ms == NULL) {
        return -1;
    }
    int y = 0;
    int mon = 0;
    int d = 0;
    int h = 0;
    int min = 0;
    int s = 0;
    if (sscanf(arg, "%d-%d-%d %d:%d:%d", &y, &mon, &d, &h, &min, &s) != 6) {
        return -1;
    }
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = y - 1900;
    tm_val.tm_mon = mon - 1;
    tm_val.tm_mday = d;
    tm_val.tm_hour = h;
    tm_val.tm_min = min;
    tm_val.tm_sec = s;
    tm_val.tm_isdst = -1;
    time_t ts = mktime(&tm_val);
    if (ts < 0) {
        return -1;
    }
    *out_ms = (uint64_t)ts * 1000ULL;
    return 0;
}

int main(int argc, char *argv[]) {
    int debug_enabled = 0;
    int latency_enabled = 0;
    int format_json_enabled = 0;
    int lba_stats_read_count_enabled = 0;
    int lba_stats_w2fr_enabled = 0;
    int lba_stats_life_cycle_enabled = 0;
    int qd_dist_enabled = 0;
    int wa_dist_enabled = 0;
    int read_size_dist_enabled = 0;
    int write_size_dist_enabled = 0;
    int trim_size_dist_enabled = 0;
    int block_size_set = 0;
    uint64_t block_size_bytes = 0ULL;
    int time_start_set = 0;
    int time_end_set = 0;
    uint64_t time_start_ms = 0ULL;
    uint64_t time_end_ms = 0ULL;
    const char *positionals[3] = {NULL, NULL, NULL};
    int positional_count = 0;
    for (int argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "-D") == 0 || strcmp(argv[argi], "--debug") == 0) {
            debug_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-l") == 0 || strcmp(argv[argi], "--latency") == 0) {
            latency_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-j") == 0 || strcmp(argv[argi], "--format-json") == 0) {
            format_json_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-r") == 0 || strcmp(argv[argi], "--read-count") == 0) {
            lba_stats_read_count_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-w") == 0 || strcmp(argv[argi], "--w2fr") == 0) {
            lba_stats_w2fr_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-c") == 0 || strcmp(argv[argi], "--life-cycle") == 0) {
            lba_stats_life_cycle_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-q") == 0 || strcmp(argv[argi], "--qd-dist") == 0) {
            qd_dist_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-a") == 0 || strcmp(argv[argi], "--wa-dist") == 0) {
            wa_dist_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-R") == 0 || strcmp(argv[argi], "--read-size-dist") == 0) {
            read_size_dist_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-W") == 0 || strcmp(argv[argi], "--write-size-dist") == 0) {
            write_size_dist_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-T") == 0 || strcmp(argv[argi], "--trim-size-dist") == 0) {
            trim_size_dist_enabled = 1;
            continue;
        }
        if (strcmp(argv[argi], "-b") == 0 || strcmp(argv[argi], "--block-size") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "%s requires a value\n", argv[argi]);
                return 1;
            }
            if (parse_u64_with_unit(argv[argi + 1], &block_size_bytes) != 0 || block_size_bytes == 0ULL) {
                fprintf(stderr, "invalid %s: %s\n", argv[argi], argv[argi + 1]);
                return 1;
            }
            block_size_set = 1;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "-S") == 0 || strcmp(argv[argi], "--time-start") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "%s requires a value in format \"YYYY-MM-DD HH:MM:SS\"\n",
                        argv[argi]);
                return 1;
            }
            if (parse_time_window_ms(argv[argi + 1], &time_start_ms) != 0) {
                fprintf(stderr, "invalid %s: %s (expected: YYYY-MM-DD HH:MM:SS)\n",
                        argv[argi], argv[argi + 1]);
                return 1;
            }
            time_start_set = 1;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "-E") == 0 || strcmp(argv[argi], "--time-end") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "%s requires a value in format \"YYYY-MM-DD HH:MM:SS\"\n",
                        argv[argi]);
                return 1;
            }
            if (parse_time_window_ms(argv[argi + 1], &time_end_ms) != 0) {
                fprintf(stderr, "invalid %s: %s (expected: YYYY-MM-DD HH:MM:SS)\n",
                        argv[argi], argv[argi + 1]);
                return 1;
            }
            time_end_set = 1;
            ++argi;
            continue;
        }

        if (argv[argi][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            return 1;
        }
        if (positional_count >= 3) {
            fprintf(stderr, "too many positional arguments\n");
            return 1;
        }
        positionals[positional_count++] = argv[argi];
    }

    if (positional_count != 3) {
        fprintf(stderr,
                "usage: %s [-D|--debug] [-j|--format-json] [-l|--latency] "
                "[-r|--read-count] [-w|--w2fr] [-c|--life-cycle] "
                "[-q|--qd-dist] [-a|--wa-dist] "
                "[-R|--read-size-dist] [-W|--write-size-dist] [-T|--trim-size-dist] "
                "[-b|--block-size <bytes|K|M|G|T>] "
                "[-S|--time-start \"YYYY-MM-DD HH:MM:SS\"] "
                "[-E|--time-end \"YYYY-MM-DD HH:MM:SS\"] "
                "<device_name> <slba[K|M|G|T]> <data_len[K|M|G|T]>\n",
                argv[0]);
        return 1;
    }
    if (time_start_set != 0 && time_end_set != 0 && time_start_ms > time_end_ms) {
        fprintf(stderr, "invalid time window: time-start must be <= time-end\n");
        return 1;
    }

    const char *device_name = positionals[0];
    // slba and data_len share the same unit parser for consistent CLI behavior.
    uint64_t slba = 0ULL;
    if (parse_u64_with_unit(positionals[1], &slba) != 0) {
        fprintf(stderr, "invalid slba: %s\n", positionals[1]);
        return 1;
    }

    uint64_t data_len = 0;
    if (parse_u64_with_unit(positionals[2], &data_len) != 0 || data_len == 0ULL) {
        fprintf(stderr, "invalid data_len: %s (examples: 128K, 64M, 1G, 1T)\n", positionals[2]);
        return 1;
    }

    nvme_read_set_debug(debug_enabled);
    nvme_read_set_format_json(format_json_enabled);
    nvme_read_set_stat_sample_collection(0);
    nvme_read_set_latency(latency_enabled);
    nvme_read_set_lba_stats_read_count(lba_stats_read_count_enabled);
    nvme_read_set_lba_stats_w2fr(lba_stats_w2fr_enabled);
    nvme_read_set_lba_stats_life_cycle(lba_stats_life_cycle_enabled);
    nvme_read_set_qd_dist(qd_dist_enabled);
    nvme_read_set_wa_dist(wa_dist_enabled);
    nvme_read_set_read_size_dist(read_size_dist_enabled);
    nvme_read_set_write_size_dist(write_size_dist_enabled);
    nvme_read_set_trim_size_dist(trim_size_dist_enabled);
    if (nvme_read_set_block_size_bytes(block_size_set != 0 ? block_size_bytes : 0ULL) != 0) {
        fprintf(stderr, "set block size failed: %s\n", strerror(errno));
        return 1;
    }
    if (nvme_read_set_time_window(time_start_set, time_start_ms,
                                  time_end_set, time_end_ms) != 0) {
        fprintf(stderr, "set time window failed: %s\n", strerror(errno));
        return 1;
    }

    if (nvme_read(device_name, slba, data_len, NULL) != 0) {
        fprintf(stderr, "nvme_read failed: %s\n", strerror(errno));
        return 1;
    }

    if (debug_enabled != 0) {
        fprintf(stderr, "nvme passthru read done. slba=%" PRIu64 " data_len=%" PRIu64 "\n",
                slba, data_len);
    }
    return 0;
}
