#define _POSIX_C_SOURCE 200809L

#include "nvme_read.h"
#include "post_action.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_DUMP_OP_READ 0x01U
#define LOG_DUMP_OP_WRITE 0x02U
#define LOG_DUMP_OP_TRIM 0x03U

typedef struct {
    FILE *out;
    uint64_t record_count;
} log_dump_ctx_t;

static int parse_time_window_ms(const char *arg, uint64_t *out_ms) {
    if (arg == NULL || out_ms == NULL) {
        return -1;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(arg, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return -1;
    }
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = second;
    tm_val.tm_isdst = -1;
    time_t ts = mktime(&tm_val);
    if (ts < 0) {
        return -1;
    }
    *out_ms = (uint64_t)ts * 1000ULL;
    return 0;
}

static const char *log_dump_op_name(uint8_t op) {
    switch (op) {
        case LOG_DUMP_OP_READ:
            return "read";
        case LOG_DUMP_OP_WRITE:
            return "write";
        case LOG_DUMP_OP_TRIM:
            return "trim";
        default:
            return "unknown";
    }
}

static int log_dump_format_time(const nvme_post_action_io_record_t *record,
                                char *buf,
                                size_t buf_len) {
    if (record == NULL || buf == NULL || buf_len == 0U) {
        return -1;
    }
    uint64_t offset_us = 0ULL;
    if (record->abs_time_us >= record->marker_abs_time_us) {
        offset_us = record->abs_time_us - record->marker_abs_time_us;
    }
    uint64_t wall_ms = record->marker_unix_time_ms + (offset_us / 1000ULL);
    uint32_t frac_us = (uint32_t)(offset_us % 1000ULL);
    time_t sec = (time_t)(wall_ms / 1000ULL);
    int ms = (int)(wall_ms % 1000ULL);
    struct tm tm_local;
    if (localtime_r(&sec, &tm_local) == NULL) {
        return -1;
    }
    int micro = ms * 1000 + (int)frac_us;
    int n = snprintf(buf,
                     buf_len,
                     "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                     tm_local.tm_year + 1900,
                     tm_local.tm_mon + 1,
                     tm_local.tm_mday,
                     tm_local.tm_hour,
                     tm_local.tm_min,
                     tm_local.tm_sec,
                     micro);
    if (n < 0 || (size_t)n >= buf_len) {
        return -1;
    }
    return 0;
}

static int log_dump_record_cb(void *ctx, const nvme_post_action_io_record_t *record) {
    log_dump_ctx_t *dump = (log_dump_ctx_t *)ctx;
    if (dump == NULL || dump->out == NULL || record == NULL) {
        errno = EINVAL;
        return -1;
    }
    char time_buf[64];
    if (log_dump_format_time(record, time_buf, sizeof(time_buf)) != 0) {
        return -1;
    }
    if (fprintf(dump->out,
                "%s %s %" PRIu64 " %" PRIu64 "\n",
                time_buf,
                log_dump_op_name(record->op),
                record->start_lba,
                record->len_lba) < 0) {
        return -1;
    }
    ++dump->record_count;
    return 0;
}

static int log_dump_is_soft_stop_errno(int saved_errno) {
    return (saved_errno == ECANCELED || saved_errno == ENODATA || saved_errno == EPIPE) ? 1 : 0;
}

static int log_dump_process_file_buffer(const unsigned char *buf,
                                        size_t len,
                                        uint64_t offset_bytes) {
    if (buf == NULL || len == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (nvme_post_action_set_base_lba(0ULL) != 0) {
        return -1;
    }
    if (nvme_post_action_process((void *)(uintptr_t)buf, (uint32_t)len, offset_bytes) != 0) {
        int saved_errno = errno;
        if (log_dump_is_soft_stop_errno(saved_errno) != 0) {
            errno = 0;
            return 0;
        }
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int log_dump_run(const char *device,
                        const char *file_path,
                        uint64_t file_offset_bytes,
                        const char *time_start,
                        const char *time_end,
                        FILE *out) {
    uint64_t start_ms = 0ULL;
    uint64_t end_ms = 0ULL;
    if (parse_time_window_ms(time_start, &start_ms) != 0 ||
        parse_time_window_ms(time_end, &end_ms) != 0 ||
        start_ms > end_ms) {
        errno = EINVAL;
        return -1;
    }

    log_dump_ctx_t dump_ctx;
    memset(&dump_ctx, 0, sizeof(dump_ctx));
    dump_ctx.out = out;

    if (nvme_read_set_print_end_reports(0) != 0 ||
        nvme_read_set_time_window(1, start_ms, 1, end_ms) != 0 ||
        nvme_post_action_set_io_record_callback(log_dump_record_cb, &dump_ctx) != 0) {
        return -1;
    }

    uint64_t slba = LOG_START_LBA;
    uint64_t data_len = 0ULL;
    if (file_path == NULL) {
        if (nvme_read_probe_log_slba_by_time(device, time_start, &slba) != 0) {
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        if (slba < LOG_START_LBA || slba >= LOG_END_LBA) {
            errno = EINVAL;
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        data_len = LOG_END_LBA - slba;
    }

    int rc = 0;
    if (file_path != NULL) {
        FILE *fp = fopen(file_path, "rb");
        if (fp == NULL) {
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        long file_size = ftell(fp);
        if (file_size < 0 ||
            (uint64_t)file_offset_bytes > (uint64_t)file_size) {
            fclose(fp);
            errno = EINVAL;
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        size_t len = (size_t)((uint64_t)file_size - file_offset_bytes);
        if (fseek(fp, (long)file_offset_bytes, SEEK_SET) != 0) {
            fclose(fp);
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        unsigned char *buf = (unsigned char *)malloc(len);
        if (buf == NULL) {
            fclose(fp);
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        if (fread(buf, 1U, len, fp) != len) {
            free(buf);
            fclose(fp);
            nvme_post_action_set_io_record_callback(NULL, NULL);
            return -1;
        }
        fclose(fp);
        rc = log_dump_process_file_buffer(buf, len, file_offset_bytes);
        free(buf);
    } else {
        rc = nvme_read(device, slba, data_len);
        if (rc != 0) {
            int saved_errno = errno;
            if (log_dump_is_soft_stop_errno(saved_errno) != 0) {
                rc = 0;
                errno = 0;
            } else {
                errno = saved_errno;
            }
        }
    }

    nvme_post_action_set_io_record_callback(NULL, NULL);

    if (rc != 0) {
        return -1;
    }
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <device> <time_start \"YYYY-MM-DD HH:MM:SS\"> "
            "<time_end \"YYYY-MM-DD HH:MM:SS\"> <output_file>\n"
            "       %s -f <input_file> [file_offset_bytes] <time_start> <time_end> <output_file>\n",
            prog,
            prog);
}

int main(int argc, char *argv[]) {
    const char *device = NULL;
    const char *file_path = NULL;
    uint64_t file_offset_bytes = 0ULL;
    const char *time_start = NULL;
    const char *time_end = NULL;
    const char *output_path = NULL;

    if (argc == 5) {
        device = argv[1];
        time_start = argv[2];
        time_end = argv[3];
        output_path = argv[4];
    } else if (argc == 6 && strcmp(argv[1], "-f") == 0) {
        file_path = argv[2];
        time_start = argv[3];
        time_end = argv[4];
        output_path = argv[5];
    } else if (argc == 7 && strcmp(argv[1], "-f") == 0) {
        file_path = argv[2];
        char *endptr = NULL;
        errno = 0;
        unsigned long long off = strtoull(argv[3], &endptr, 10);
        if (errno != 0 || endptr == argv[3] || *endptr != '\0') {
            print_usage(argv[0]);
            return 1;
        }
        file_offset_bytes = (uint64_t)off;
        time_start = argv[4];
        time_end = argv[5];
        output_path = argv[6];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    if ((device == NULL && file_path == NULL) ||
        (device != NULL && file_path != NULL) ||
        time_start == NULL || time_end == NULL || output_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    FILE *out = fopen(output_path, "w");
    if (out == NULL) {
        fprintf(stderr, "failed to open output file %s: %s\n", output_path, strerror(errno));
        return 1;
    }

    int rc = log_dump_run(device, file_path, file_offset_bytes, time_start, time_end, out);
    if (fclose(out) != 0 && rc == 0) {
        rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "log_dump failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
