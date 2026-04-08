#include "nvme_read.h"
#include "post_action.h"

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
    int read_count_stats_enabled = 0;
    int w2fr_stats_enabled = 0;
    int life_cycle_stats_enabled = 0;
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
        if (strcmp(argv[argi], "-r") == 0 || strcmp(argv[argi], "--read-count") == 0) {
            read_count_stats_enabled = 1;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "-w") == 0 || strcmp(argv[argi], "--w2fr") == 0) {
            w2fr_stats_enabled = 1;
            ++argi;
            continue;
        }
        if (strcmp(argv[argi], "-c") == 0 || strcmp(argv[argi], "--life-cycle") == 0) {
            life_cycle_stats_enabled = 1;
            ++argi;
            continue;
        }
        break;
    }

    uint64_t split_bytes = 0ULL;
    int use_split = 0;
    while (argi < argc) {
        if (strcmp(argv[argi], "--split-bytes") == 0) {
            if ((argi + 1) >= argc) {
                fprintf(stderr, "--split-bytes requires a value\n");
                return 2;
            }
            if (parse_u64_with_unit(argv[argi + 1], &split_bytes) != 0 || split_bytes == 0ULL) {
                fprintf(stderr, "invalid --split-bytes: %s\n", argv[argi + 1]);
                return 2;
            }
            use_split = 1;
            argi += 2;
            continue;
        }
        break;
    }

    if ((argc - argi) != 1 && (argc - argi) != 2) {
        fprintf(stderr,
                "usage: %s [-D|--debug] [-l|--latency] [-r|--read-count] [-w|--w2fr] "
                "[-c|--life-cycle] [--split-bytes N] <input_file> [offset_bytes]\n",
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
    nvme_read_set_lba_stats_read_count(read_count_stats_enabled);
    nvme_read_set_lba_stats_w2fr(w2fr_stats_enabled);
    nvme_read_set_lba_stats_life_cycle(life_cycle_stats_enabled);
    int rc = 0;
    if (use_split != 0) {
        uint64_t cursor = 0ULL;
        while (cursor < (uint64_t)len) {
            uint64_t remaining = (uint64_t)len - cursor;
            uint64_t chunk = remaining < split_bytes ? remaining : split_bytes;
            if (chunk > (uint64_t)UINT32_MAX) {
                fprintf(stderr, "split chunk too large: %" PRIu64 "\n", chunk);
                free(buf);
                return 2;
            }
            rc = nvme_post_action_process(buf + cursor, (uint32_t)chunk, offset_bytes + cursor);
            if (rc != 0) {
                break;
            }
            cursor += chunk;
        }
    } else {
        rc = nvme_post_action_process(buf, (uint32_t)len, offset_bytes);
    }
    if (rc != 0) {
        if (errno == ECANCELED) {
            fprintf(stderr,
                    "post action soft-stop: overwrite detected, stop further parse at offset=%" PRIu64 "\n",
                    offset_bytes);
        } else if (errno == ENODATA) {
            fprintf(stderr,
                    "post action soft-stop: all-zero record detected, stop further parse at offset=%" PRIu64 "\n",
                    offset_bytes);
        } else {
            fprintf(stderr, "post action failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
    }

    fprintf(stderr, "post action success: file=%s bytes=%zu offset=%" PRIu64 "\n",
            input_file, len, offset_bytes);
    if (latency_enabled != 0) {
        nvme_post_action_print_latency_report();
    }
    if (read_count_stats_enabled != 0 ||
        w2fr_stats_enabled != 0 ||
        life_cycle_stats_enabled != 0) {
        nvme_post_action_print_lba_stats_report();
    }
    free(buf);
    return 0;
}
