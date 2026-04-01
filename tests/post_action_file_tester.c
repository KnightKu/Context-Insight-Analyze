#include "nvme_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u64(const char *text, uint64_t *value) {
    if (text == NULL || value == NULL || *text == '\0') {
        return -1;
    }
    char *endptr = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &endptr, 10);
    if (errno != 0 || endptr == text || *endptr != '\0') {
        return -1;
    }
    *value = (uint64_t)v;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2 && argc != 3 && argc != 6) {
        fprintf(stderr,
                "usage: %s <input_file> [offset_bytes]\n"
                "   or: %s <input_file> <offset_bytes> <csv_path> <start_bucket> <bucket_count>\n",
                argv[0], argv[0]);
        return 2;
    }

    uint64_t offset_bytes = 0ULL;
    if ((argc == 3 || argc == 6) && parse_u64(argv[2], &offset_bytes) != 0) {
        fprintf(stderr, "invalid offset_bytes: %s\n", argv[2]);
        return 2;
    }

    const char *input_file = argv[1];
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

    nvme_read_set_latency(1);
    int rc = nvme_post_action_process(buf, (uint32_t)len, offset_bytes);
    if (rc != 0) {
        fprintf(stderr, "post action failed: %s\n", strerror(errno));
        free(buf);
        return 1;
    }

    if (argc == 6) {
        const char *csv_path = argv[3];
        uint64_t start_bucket = 0ULL;
        uint64_t bucket_count = 0ULL;
        if (parse_u64(argv[4], &start_bucket) != 0) {
            fprintf(stderr, "invalid start_bucket: %s\n", argv[4]);
            free(buf);
            return 2;
        }
        if (parse_u64(argv[5], &bucket_count) != 0) {
            fprintf(stderr, "invalid bucket_count: %s\n", argv[5]);
            free(buf);
            return 2;
        }
        if (nvme_post_action_export_stats_csv(csv_path, start_bucket, bucket_count) != 0) {
            fprintf(stderr, "export csv failed: %s\n", strerror(errno));
            free(buf);
            return 1;
        }
        fprintf(stderr,
                "csv export success: path=%s start_bucket=%" PRIu64 " bucket_count=%" PRIu64 "\n",
                csv_path, start_bucket, bucket_count);
    }

    fprintf(stderr, "post action success: file=%s bytes=%zu offset=%" PRIu64 "\n",
            input_file, len, offset_bytes);
    free(buf);
    return 0;
}
