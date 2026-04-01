#include "nvme_read.h"
#include "post_action_stats.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

int nvme_post_action_export_stats_csv(const char *csv_path,
                                      uint64_t start_bucket,
                                      uint64_t bucket_count) {
    if (csv_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (nvme_post_action_stats_init(0U) != 0) {
        errno = ENODEV;
        return -1;
    }

    uint64_t total_buckets = 0ULL;
    if (nvme_post_action_stats_get_bucket_count(&total_buckets) != 0) {
        errno = ENODEV;
        return -1;
    }
    if (start_bucket >= total_buckets) {
        errno = ERANGE;
        return -1;
    }

    uint64_t end_exclusive = start_bucket + bucket_count;
    if (end_exclusive < start_bucket || end_exclusive > total_buckets) {
        end_exclusive = total_buckets;
    }

    FILE *fp = fopen(csv_path, "w");
    if (fp == NULL) {
        return -1;
    }

    if (fprintf(fp,
                "bucket_index,read_count,write_to_first_read_latency_code,life_cycle_latency_code\n") < 0) {
        int saved_errno = errno == 0 ? EIO : errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    for (uint64_t i = start_bucket; i < end_exclusive; ++i) {
        nvme_post_action_lba_stat_t stat;
        if (nvme_post_action_stats_get_bucket(i, &stat) != 0) {
            int saved_errno = errno == 0 ? EIO : errno;
            fclose(fp);
            errno = saved_errno;
            return -1;
        }
        if (fprintf(fp, "%llu,%u,%u,%u\n",
                    (unsigned long long)i,
                    (unsigned int)stat.read_count,
                    (unsigned int)stat.write_to_first_read_latency,
                    (unsigned int)stat.life_cycle_latency) < 0) {
            int saved_errno = errno == 0 ? EIO : errno;
            fclose(fp);
            errno = saved_errno;
            return -1;
        }
    }

    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

int nvme_post_action_export_advanced_life_csv(const char *csv_path,
                                              uint64_t min_range_kib,
                                              uint64_t max_range_kib) {
    if (csv_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (nvme_post_action_stats_init(0U) != 0) {
        errno = ENODEV;
        return -1;
    }
    if (min_range_kib == 0ULL || max_range_kib == 0ULL || min_range_kib > max_range_kib) {
        errno = EINVAL;
        return -1;
    }

    FILE *fp = fopen(csv_path, "w");
    if (fp == NULL) {
        return -1;
    }

    if (fprintf(fp, "range_kib,life_cycle_code,count\n") < 0) {
        int saved_errno = errno == 0 ? EIO : errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    uint64_t rows = 0ULL;
    for (uint64_t range_kib = min_range_kib; range_kib <= max_range_kib; range_kib <<= 1U) {
        for (uint32_t code = 0U; code <= NVME_POST_ACTION_LATENCY_CODE_MAX; ++code) {
            uint64_t count = 0ULL;
            if (nvme_post_action_stats_get_advanced_life_count(range_kib, (uint16_t)code, &count) != 0) {
                continue;
            }
            if (count == 0ULL) {
                continue;
            }
            if (fprintf(fp, "%llu,%u,%llu\n",
                        (unsigned long long)range_kib,
                        (unsigned int)code,
                        (unsigned long long)count) < 0) {
                int saved_errno = errno == 0 ? EIO : errno;
                fclose(fp);
                errno = saved_errno;
                return -1;
            }
            ++rows;
        }
        if (range_kib > (UINT64_MAX >> 1U)) {
            break;
        }
    }

    if (rows == 0ULL) {
        // Keep header-only CSV valid when no samples exist.
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

int nvme_post_action_export_advanced_life_cycle_csv(const char *csv_path) {
    return nvme_post_action_export_advanced_life_csv(csv_path, 4ULL, UINT64_MAX);
}
