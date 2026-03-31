#define _POSIX_C_SOURCE 200809L
#include "nvme_read.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef NVME_POST_ACTION_DEBUG
#define NVME_POST_ACTION_DEBUG 0
#endif

#define NVME_POST_ACTION_RECORD_BYTES_SHORT 8U
#define NVME_POST_ACTION_RECORD_BYTES_LONG 16U

#define NVME_POST_ACTION_OP_READ 0x01U
#define NVME_POST_ACTION_OP_WRITE 0x02U
#define NVME_POST_ACTION_OP_TRIM 0x03U
#define NVME_POST_ACTION_OP_STAT 0x0FU
#define NVME_POST_ACTION_OP_MARKER 0xFFU

#define NVME_POST_ACTION_START_LBA_MASK 0xFFFFFFFFFFULL
#define NVME_POST_ACTION_U24_MASK 0xFFFFFFU
#define NVME_POST_ACTION_U56_MASK 0x00FFFFFFFFFFFFFFULL
#define NVME_READ_PIPELINE_SLOTS 4U

typedef struct {
    uint8_t op;
    uint64_t offset_bytes;
    uint32_t record_index;
} nvme_post_action_record_meta_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint64_t start_lba;
    uint16_t length;
    uint32_t latency;
    uint32_t time_rel;
} nvme_post_action_rw_t;

typedef struct {
    uint64_t start_lba;
    uint8_t range_index;
    uint32_t length;
    uint32_t time_rel;
} nvme_post_action_trim_range_t;

#define NVME_POST_ACTION_TRIM_MAX_RANGES 256U

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint16_t total_ranges;
    uint16_t range_count;
    nvme_post_action_trim_range_t ranges[NVME_POST_ACTION_TRIM_MAX_RANGES];
} nvme_post_action_trim_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint16_t qd;
    uint8_t wa;
    uint32_t time_rel;
} nvme_post_action_stat_t;

typedef struct {
    nvme_post_action_record_meta_t meta;
    uint64_t abs_time;
} nvme_post_action_marker_t;

typedef struct {
    uint64_t start_lba;
    uint8_t total_ranges;
    uint8_t range_index;
    uint32_t length;
    uint32_t time_rel;
} nvme_post_action_trim_record_fields_t;

typedef struct {
    uint64_t marker_abs_time_us;
    int has_marker;
} nvme_post_action_time_ref_t;

typedef struct {
    void *buf;
    uint32_t len;
    uint64_t offset;
} nvme_pipeline_slot_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cv_free;
    pthread_cond_t cv_ready;
    nvme_pipeline_slot_t slots[NVME_READ_PIPELINE_SLOTS];
    uint32_t free_q[NVME_READ_PIPELINE_SLOTS];
    uint32_t ready_q[NVME_READ_PIPELINE_SLOTS];
    uint32_t free_head;
    uint32_t free_tail;
    uint32_t free_count;
    uint32_t ready_head;
    uint32_t ready_tail;
    uint32_t ready_count;
    int producer_done;
    int stop;
    int producer_failed;
    int producer_errno;
    int worker_failed;
    int worker_errno;
    uint64_t processed_bytes;
} nvme_pipeline_state_t;

typedef struct {
    nvme_pipeline_state_t *state;
    int nvme_fd;
    uint32_t sector_size;
    uint64_t slba;
    uint64_t data_len;
    uint64_t read_chunk_bytes;
} nvme_pipeline_reader_args_t;

typedef struct {
    nvme_pipeline_state_t *state;
} nvme_pipeline_worker_args_t;

static int default_post_action(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes);

static nvme_read_post_action_t g_post_action = default_post_action;
static void *g_post_action_ctx = NULL;
static int g_nvme_read_debug = 0;
static pthread_mutex_t g_post_action_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_post_action_invalid_records = 0ULL;

static void post_action_reset_invalid_count(void) {
    pthread_mutex_lock(&g_post_action_stats_mutex);
    g_post_action_invalid_records = 0ULL;
    pthread_mutex_unlock(&g_post_action_stats_mutex);
}

static void post_action_add_invalid_count(uint64_t delta) {
    if (delta == 0ULL) {
        return;
    }
    pthread_mutex_lock(&g_post_action_stats_mutex);
    g_post_action_invalid_records += delta;
    pthread_mutex_unlock(&g_post_action_stats_mutex);
}

static uint64_t post_action_get_invalid_count(void) {
    pthread_mutex_lock(&g_post_action_stats_mutex);
    uint64_t v = g_post_action_invalid_records;
    pthread_mutex_unlock(&g_post_action_stats_mutex);
    return v;
}

static int resolve_abs_time_us(uint32_t time_rel_us,
                               nvme_post_action_time_ref_t *time_ref,
                               uint64_t *abs_time_us_out) {
    if (time_ref == NULL || abs_time_us_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (time_ref->has_marker == 0) {
        errno = EINVAL;
        return -1;
    }
    *abs_time_us_out = time_ref->marker_abs_time_us + (uint64_t)time_rel_us;
    return 0;
}

// Fast unaligned 64-bit little-endian load.
static inline uint64_t load_le64_u(const unsigned char *p) {
    uint64_t v = 0ULL;
    memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

static void pipeline_push_free_locked(nvme_pipeline_state_t *state, uint32_t slot_idx) {
    state->free_q[state->free_tail] = slot_idx;
    state->free_tail = (state->free_tail + 1U) % NVME_READ_PIPELINE_SLOTS;
    ++state->free_count;
}

static void pipeline_push_ready_locked(nvme_pipeline_state_t *state, uint32_t slot_idx) {
    state->ready_q[state->ready_tail] = slot_idx;
    state->ready_tail = (state->ready_tail + 1U) % NVME_READ_PIPELINE_SLOTS;
    ++state->ready_count;
}

static int pipeline_pop_free_wait(nvme_pipeline_state_t *state, uint32_t *slot_idx) {
    pthread_mutex_lock(&state->mutex);
    while (state->free_count == 0U && state->stop == 0) {
        pthread_cond_wait(&state->cv_free, &state->mutex);
    }
    if (state->stop != 0) {
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }
    *slot_idx = state->free_q[state->free_head];
    state->free_head = (state->free_head + 1U) % NVME_READ_PIPELINE_SLOTS;
    --state->free_count;
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static int pipeline_pop_ready_wait(nvme_pipeline_state_t *state, uint32_t *slot_idx) {
    pthread_mutex_lock(&state->mutex);
    while (state->ready_count == 0U && state->producer_done == 0 && state->stop == 0) {
        pthread_cond_wait(&state->cv_ready, &state->mutex);
    }
    if (state->ready_count == 0U) {
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }
    *slot_idx = state->ready_q[state->ready_head];
    state->ready_head = (state->ready_head + 1U) % NVME_READ_PIPELINE_SLOTS;
    --state->ready_count;
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static int pipeline_state_init(nvme_pipeline_state_t *state, uint64_t slot_bytes) {
    memset(state, 0, sizeof(*state));
    int rc = pthread_mutex_init(&state->mutex, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&state->cv_free, NULL);
    if (rc != 0) {
        errno = rc;
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    rc = pthread_cond_init(&state->cv_ready, NULL);
    if (rc != 0) {
        errno = rc;
        pthread_cond_destroy(&state->cv_free);
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }

    for (uint32_t i = 0; i < NVME_READ_PIPELINE_SLOTS; ++i) {
        state->free_q[i] = i;
        if (posix_memalign(&state->slots[i].buf, 4096, (size_t)slot_bytes) != 0) {
            errno = ENOMEM;
            for (uint32_t j = 0; j < i; ++j) {
                free(state->slots[j].buf);
                state->slots[j].buf = NULL;
            }
            pthread_cond_destroy(&state->cv_ready);
            pthread_cond_destroy(&state->cv_free);
            pthread_mutex_destroy(&state->mutex);
            return -1;
        }
        state->slots[i].len = 0U;
        state->slots[i].offset = 0ULL;
    }
    state->free_head = 0U;
    state->free_tail = 0U;
    state->free_count = NVME_READ_PIPELINE_SLOTS;
    return 0;
}

static void pipeline_state_destroy(nvme_pipeline_state_t *state) {
    for (uint32_t i = 0; i < NVME_READ_PIPELINE_SLOTS; ++i) {
        free(state->slots[i].buf);
        state->slots[i].buf = NULL;
    }
    pthread_cond_destroy(&state->cv_ready);
    pthread_cond_destroy(&state->cv_free);
    pthread_mutex_destroy(&state->mutex);
}

static void *pipeline_reader_thread(void *arg) {
    nvme_pipeline_reader_args_t *args = (nvme_pipeline_reader_args_t *)arg;
    nvme_pipeline_state_t *state = args->state;
    uint64_t offset = 0ULL;

    while (offset < args->data_len) {
        uint32_t slot_idx = 0U;
        if (pipeline_pop_free_wait(state, &slot_idx) != 0) {
            break;
        }
        nvme_pipeline_slot_t *slot = &state->slots[slot_idx];
        uint64_t remaining = args->data_len - offset;
        uint32_t chunk_size = (uint32_t)(remaining > args->read_chunk_bytes ? args->read_chunk_bytes : remaining);
        uint64_t chunk_lba = offset / (uint64_t)args->sector_size;
        uint64_t backup_lba = args->slba + chunk_lba;

        struct nvme_passthru_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = 0x02;      // NVM Read
        cmd.nsid = 1;
        cmd.addr = (uint64_t)(uintptr_t)slot->buf;
        cmd.data_len = chunk_size;
        cmd.cdw10 = (uint32_t)(chunk_lba & 0xFFFFFFFFULL);
        cmd.cdw11 = (uint32_t)((chunk_lba >> 32) & 0xFFFFFFFFULL);
        cmd.cdw12 = (uint32_t)(chunk_size / (uint64_t)args->sector_size) - 1U;
        cmd.cdw14 = (uint32_t)(backup_lba & 0xFFFFFFFFULL);
        cmd.cdw15 = (uint32_t)((backup_lba >> 32) & 0xFFFFFFFFULL);

        if (ioctl(args->nvme_fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
            int saved_errno = errno == 0 ? EIO : errno;
            fprintf(stderr, "ioctl failed at offset=%llu: %s\n",
                    (unsigned long long)offset, strerror(saved_errno));
            pthread_mutex_lock(&state->mutex);
            state->producer_failed = 1;
            state->producer_errno = saved_errno;
            state->producer_done = 1;
            state->stop = 1;
            pipeline_push_free_locked(state, slot_idx);
            pthread_cond_broadcast(&state->cv_free);
            pthread_cond_broadcast(&state->cv_ready);
            pthread_mutex_unlock(&state->mutex);
            return NULL;
        }

        pthread_mutex_lock(&state->mutex);
        if (state->stop != 0) {
            pipeline_push_free_locked(state, slot_idx);
            pthread_cond_signal(&state->cv_free);
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        slot->len = chunk_size;
        slot->offset = offset;
        pipeline_push_ready_locked(state, slot_idx);
        pthread_cond_signal(&state->cv_ready);
        pthread_mutex_unlock(&state->mutex);

        offset += (uint64_t)chunk_size;
    }

    pthread_mutex_lock(&state->mutex);
    state->producer_done = 1;
    pthread_cond_broadcast(&state->cv_ready);
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

static void *pipeline_worker_thread(void *arg) {
    nvme_pipeline_worker_args_t *args = (nvme_pipeline_worker_args_t *)arg;
    nvme_pipeline_state_t *state = args->state;

    while (1) {
        uint32_t slot_idx = 0U;
        if (pipeline_pop_ready_wait(state, &slot_idx) != 0) {
            break;
        }
        nvme_pipeline_slot_t *slot = &state->slots[slot_idx];
        if (nvme_post_action_process(slot->buf, slot->len, slot->offset) != 0) {
            int saved_errno = errno == 0 ? EIO : errno;
            fprintf(stderr, "post action failed at offset=%llu: %s\n",
                    (unsigned long long)slot->offset, strerror(saved_errno));
            pthread_mutex_lock(&state->mutex);
            state->worker_failed = 1;
            state->worker_errno = saved_errno;
            state->stop = 1;
            pipeline_push_free_locked(state, slot_idx);
            pthread_cond_broadcast(&state->cv_free);
            pthread_cond_broadcast(&state->cv_ready);
            pthread_mutex_unlock(&state->mutex);
            return NULL;
        }

        pthread_mutex_lock(&state->mutex);
        state->processed_bytes += (uint64_t)slot->len;
        pipeline_push_free_locked(state, slot_idx);
        pthread_cond_signal(&state->cv_free);
        pthread_mutex_unlock(&state->mutex);
    }
    return NULL;
}

static int run_read_post_pipeline(int nvme_fd,
                                  uint32_t sector_size,
                                  uint64_t slba,
                                  uint64_t data_len,
                                  uint64_t read_chunk_bytes,
                                  uint64_t *processed_bytes_out) {
    if (processed_bytes_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    nvme_pipeline_state_t state;
    if (pipeline_state_init(&state, read_chunk_bytes) != 0) {
        return -1;
    }

    nvme_pipeline_reader_args_t reader_args;
    memset(&reader_args, 0, sizeof(reader_args));
    reader_args.state = &state;
    reader_args.nvme_fd = nvme_fd;
    reader_args.sector_size = sector_size;
    reader_args.slba = slba;
    reader_args.data_len = data_len;
    reader_args.read_chunk_bytes = read_chunk_bytes;

    nvme_pipeline_worker_args_t worker_args;
    memset(&worker_args, 0, sizeof(worker_args));
    worker_args.state = &state;

    pthread_t reader_thread;
    pthread_t worker_thread;
    int rc_reader = pthread_create(&reader_thread, NULL, pipeline_reader_thread, &reader_args);
    if (rc_reader != 0) {
        errno = rc_reader;
        pipeline_state_destroy(&state);
        return -1;
    }
    int rc_worker = pthread_create(&worker_thread, NULL, pipeline_worker_thread, &worker_args);
    if (rc_worker != 0) {
        errno = rc_worker;
        pthread_mutex_lock(&state.mutex);
        state.stop = 1;
        state.producer_done = 1;
        pthread_cond_broadcast(&state.cv_free);
        pthread_cond_broadcast(&state.cv_ready);
        pthread_mutex_unlock(&state.mutex);
        pthread_join(reader_thread, NULL);
        pipeline_state_destroy(&state);
        return -1;
    }

    pthread_join(reader_thread, NULL);
    pthread_join(worker_thread, NULL);

    int failed = 0;
    int saved_errno = 0;
    pthread_mutex_lock(&state.mutex);
    if (state.worker_failed != 0) {
        failed = 1;
        saved_errno = state.worker_errno;
    } else if (state.producer_failed != 0) {
        failed = 1;
        saved_errno = state.producer_errno;
    }
    *processed_bytes_out = state.processed_bytes;
    pthread_mutex_unlock(&state.mutex);

    pipeline_state_destroy(&state);
    if (failed != 0) {
        errno = (saved_errno == 0) ? EIO : saved_errno;
        return -1;
    }
    return 0;
}

static uint16_t trim_total_ranges_from_raw(uint8_t raw_total_ranges) {
    // Protocol defines max ranges as 256 while field width is 1 byte.
    // Treat 0 as 256 to allow representing the upper bound.
    return raw_total_ranges == 0U ? NVME_POST_ACTION_TRIM_MAX_RANGES : (uint16_t)raw_total_ranges;
}

static int parse_rw_record(uint64_t record_lo,
                           uint64_t record_hi,
                           uint8_t op,
                           uint64_t offset_bytes,
                           uint32_t record_index,
                           nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_rw_t parsed;
    parsed.meta.op = op;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    parsed.start_lba = (record_lo >> 8U) & NVME_POST_ACTION_START_LBA_MASK;
    parsed.length = (uint16_t)(record_lo >> 48U);
    uint16_t reserved = (uint16_t)(record_hi & 0xFFFFU);
    parsed.latency = (uint32_t)((record_hi >> 16U) & NVME_POST_ACTION_U24_MASK);
    parsed.time_rel = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);
    uint64_t abs_time_us = 0ULL;

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid rw reserved: offset=%llu record=%u op=0x%02x reserved=0x%04x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)op,
                (unsigned int)reserved);
        return -1;
    }

    if (resolve_abs_time_us(parsed.time_rel, time_ref, &abs_time_us) != 0) {
        fprintf(stderr,
                "post action missing marker for rw time: offset=%llu record=%u op=0x%02x time_rel=%u\n",
                (unsigned long long)offset_bytes,
                (unsigned int)record_index,
                (unsigned int)op,
                (unsigned int)parsed.time_rel);
        return -1;
    }

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action %s: offset=%llu record=%u start_lba=%llu len=%u latency=%u time_rel=%u abs_time_us=%llu\n",
            parsed.meta.op == NVME_POST_ACTION_OP_READ ? "read" : "write",
            (unsigned long long)parsed.meta.offset_bytes,
            (unsigned int)parsed.meta.record_index,
            (unsigned long long)parsed.start_lba,
            (unsigned int)parsed.length,
            (unsigned int)parsed.latency,
            (unsigned int)parsed.time_rel,
            (unsigned long long)abs_time_us);
#else
    (void)parsed;
    (void)abs_time_us;
#endif
    return 0;
}

static int decode_trim_record_fields(uint64_t record_lo,
                                     uint64_t record_hi,
                                     uint64_t offset_bytes,
                                     uint32_t record_index,
                                     nvme_post_action_trim_record_fields_t *fields) {
    if (fields == NULL) {
        errno = EINVAL;
        return -1;
    }

    fields->start_lba = (record_lo >> 8U) & NVME_POST_ACTION_START_LBA_MASK;
    fields->total_ranges = (uint8_t)(record_lo >> 48U);
    fields->range_index = (uint8_t)(record_lo >> 56U);
    uint8_t reserved = (uint8_t)(record_hi & 0xFFU);
    fields->length = (uint32_t)((record_hi >> 8U) & 0xFFFFFFFFU);
    fields->time_rel = (uint32_t)((record_hi >> 40U) & NVME_POST_ACTION_U24_MASK);

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid trim reserved: offset=%llu record=%u reserved=0x%02x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)reserved);
        return -1;
    }
    return 0;
}

static int parse_trim_group(const unsigned char *bytes,
                            uint32_t data_len,
                            uint32_t cursor,
                            uint64_t base_offset_bytes,
                            uint32_t record_index,
                            nvme_post_action_time_ref_t *time_ref,
                            uint32_t *consumed_bytes,
                            uint32_t *consumed_records) {
    if (bytes == NULL || consumed_bytes == NULL || consumed_records == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (time_ref == NULL || time_ref->has_marker == 0) {
        errno = EINVAL;
        fprintf(stderr,
                "post action missing marker for trim time: offset=%llu record=%u\n",
                (unsigned long long)(base_offset_bytes + (uint64_t)cursor),
                (unsigned int)record_index);
        return -1;
    }

    if ((data_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
        errno = EINVAL;
        fprintf(stderr,
                "post action truncated trim group head: offset=%llu record=%u remain=%u need=%u\n",
                (unsigned long long)(base_offset_bytes + (uint64_t)cursor),
                (unsigned int)record_index,
                (unsigned int)(data_len - cursor),
                (unsigned int)NVME_POST_ACTION_RECORD_BYTES_LONG);
        return -1;
    }

    nvme_post_action_trim_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.meta.op = NVME_POST_ACTION_OP_TRIM;
    parsed.meta.offset_bytes = base_offset_bytes + (uint64_t)cursor;
    parsed.meta.record_index = record_index;

    uint64_t first_lo = load_le64_u(bytes + cursor);
    uint64_t first_hi = load_le64_u(bytes + cursor + 8U);
    if (((uint8_t)(first_lo & 0xFFU)) != NVME_POST_ACTION_OP_TRIM) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid trim group head op: offset=%llu record=%u op=0x%02x\n",
                (unsigned long long)parsed.meta.offset_bytes,
                (unsigned int)parsed.meta.record_index,
                (unsigned int)(first_lo & 0xFFU));
        return -1;
    }

    nvme_post_action_trim_record_fields_t first_fields;
    if (decode_trim_record_fields(first_lo, first_hi, parsed.meta.offset_bytes, parsed.meta.record_index,
                                  &first_fields) != 0) {
        return -1;
    }

    parsed.total_ranges = trim_total_ranges_from_raw(first_fields.total_ranges);
    if (parsed.total_ranges == 0U || parsed.total_ranges > NVME_POST_ACTION_TRIM_MAX_RANGES) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid trim total_ranges: offset=%llu record=%u raw=%u resolved=%u\n",
                (unsigned long long)parsed.meta.offset_bytes,
                (unsigned int)parsed.meta.record_index,
                (unsigned int)first_fields.total_ranges,
                (unsigned int)parsed.total_ranges);
        return -1;
    }

    for (uint16_t i = 0; i < parsed.total_ranges; ++i) {
        uint32_t group_cursor = cursor + ((uint32_t)i * NVME_POST_ACTION_RECORD_BYTES_LONG);
        if ((data_len - group_cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
            errno = EINVAL;
            fprintf(stderr,
                    "post action truncated trim range: offset=%llu record=%u range=%u remain=%u need=%u\n",
                    (unsigned long long)(base_offset_bytes + (uint64_t)group_cursor),
                    (unsigned int)(record_index + (uint32_t)i),
                    (unsigned int)i,
                    (unsigned int)(data_len - group_cursor),
                    (unsigned int)NVME_POST_ACTION_RECORD_BYTES_LONG);
            return -1;
        }

        uint64_t record_lo = load_le64_u(bytes + group_cursor);
        uint8_t op = (uint8_t)(record_lo & 0xFFU);
        if (op != NVME_POST_ACTION_OP_TRIM) {
            errno = EINVAL;
            fprintf(stderr,
                    "post action trim group broken: offset=%llu record=%u range=%u op=0x%02x\n",
                    (unsigned long long)(base_offset_bytes + (uint64_t)group_cursor),
                    (unsigned int)(record_index + (uint32_t)i),
                    (unsigned int)i,
                    (unsigned int)op);
            return -1;
        }

        uint64_t record_hi = load_le64_u(bytes + group_cursor + 8U);
        nvme_post_action_trim_record_fields_t fields;
        if (decode_trim_record_fields(record_lo, record_hi,
                                      base_offset_bytes + (uint64_t)group_cursor,
                                      record_index + (uint32_t)i, &fields) != 0) {
            return -1;
        }

        if (trim_total_ranges_from_raw(fields.total_ranges) != parsed.total_ranges) {
            errno = EINVAL;
            fprintf(stderr,
                    "post action trim group inconsistent total_ranges: offset=%llu record=%u "
                    "expected=%u got=%u(raw=%u)\n",
                    (unsigned long long)(base_offset_bytes + (uint64_t)group_cursor),
                    (unsigned int)(record_index + (uint32_t)i),
                    (unsigned int)parsed.total_ranges,
                    (unsigned int)trim_total_ranges_from_raw(fields.total_ranges),
                    (unsigned int)fields.total_ranges);
            return -1;
        }

        parsed.ranges[i].start_lba = fields.start_lba;
        parsed.ranges[i].range_index = fields.range_index;
        parsed.ranges[i].length = fields.length;
        parsed.ranges[i].time_rel = fields.time_rel;
        uint64_t abs_time_us = 0ULL;
        if (resolve_abs_time_us(fields.time_rel, time_ref, &abs_time_us) != 0) {
            fprintf(stderr,
                    "post action missing marker for trim range time: offset=%llu record=%u range=%u\n",
                    (unsigned long long)(base_offset_bytes + (uint64_t)group_cursor),
                    (unsigned int)(record_index + (uint32_t)i),
                    (unsigned int)i);
            return -1;
        }
#if NVME_POST_ACTION_DEBUG
        (void)abs_time_us;
#endif
    }
    parsed.range_count = parsed.total_ranges;

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action trim group: offset=%llu record=%u total_ranges=%u\n",
            (unsigned long long)parsed.meta.offset_bytes,
            (unsigned int)parsed.meta.record_index,
            (unsigned int)parsed.total_ranges);
    for (uint16_t i = 0; i < parsed.range_count; ++i) {
        uint64_t abs_time_us = time_ref->marker_abs_time_us + (uint64_t)parsed.ranges[i].time_rel;
        fprintf(stderr,
                "  trim range[%u]: start_lba=%llu range_index=%u len=%u time_rel=%u abs_time_us=%llu\n",
                (unsigned int)i,
                (unsigned long long)parsed.ranges[i].start_lba,
                (unsigned int)parsed.ranges[i].range_index,
                (unsigned int)parsed.ranges[i].length,
                (unsigned int)parsed.ranges[i].time_rel,
                (unsigned long long)abs_time_us);
    }
#else
    (void)parsed;
#endif

    *consumed_bytes = (uint32_t)parsed.total_ranges * NVME_POST_ACTION_RECORD_BYTES_LONG;
    *consumed_records = (uint32_t)parsed.total_ranges;
    return 0;
}

static int parse_stat_record(uint64_t record_lo,
                             uint64_t offset_bytes,
                             uint32_t record_index,
                             nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_stat_t parsed;
    parsed.meta.op = NVME_POST_ACTION_OP_STAT;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    uint8_t reserved = (uint8_t)((record_lo >> 8U) & 0xFFU);
    parsed.qd = (uint16_t)((record_lo >> 16U) & 0xFFFFU);
    parsed.wa = (uint8_t)((record_lo >> 32U) & 0xFFU);
    parsed.time_rel = (uint32_t)((record_lo >> 40U) & NVME_POST_ACTION_U24_MASK);
    uint64_t abs_time_us = 0ULL;

    if (reserved != 0U) {
        errno = EINVAL;
        fprintf(stderr,
                "post action invalid stat reserved: offset=%llu record=%u reserved=0x%02x\n",
                (unsigned long long)offset_bytes, (unsigned int)record_index, (unsigned int)reserved);
        return -1;
    }

    if (resolve_abs_time_us(parsed.time_rel, time_ref, &abs_time_us) != 0) {
        fprintf(stderr,
                "post action missing marker for stat time: offset=%llu record=%u time_rel=%u\n",
                (unsigned long long)offset_bytes,
                (unsigned int)record_index,
                (unsigned int)parsed.time_rel);
        return -1;
    }

#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action stat: offset=%llu record=%u qd=%u wa=%u time_rel=%u abs_time_us=%llu\n",
            (unsigned long long)parsed.meta.offset_bytes,
            (unsigned int)parsed.meta.record_index,
            (unsigned int)parsed.qd,
            (unsigned int)parsed.wa,
            (unsigned int)parsed.time_rel,
            (unsigned long long)abs_time_us);
#else
    (void)parsed;
    (void)abs_time_us;
#endif
    return 0;
}

static int parse_marker_record(uint64_t record_lo,
                               uint64_t offset_bytes,
                               uint32_t record_index,
                               nvme_post_action_time_ref_t *time_ref) {
    nvme_post_action_marker_t parsed;
    parsed.meta.op = NVME_POST_ACTION_OP_MARKER;
    parsed.meta.offset_bytes = offset_bytes;
    parsed.meta.record_index = record_index;
    parsed.abs_time = (record_lo >> 8U) & NVME_POST_ACTION_U56_MASK;
    if (time_ref == NULL) {
        errno = EINVAL;
        return -1;
    }
    time_ref->marker_abs_time_us = parsed.abs_time;
    time_ref->has_marker = 1;
#if NVME_POST_ACTION_DEBUG
    fprintf(stderr,
            "post action marker: offset=%llu record=%u abs_time=%llu\n",
            (unsigned long long)parsed.meta.offset_bytes,
            (unsigned int)parsed.meta.record_index,
            (unsigned long long)parsed.abs_time);
#else
    (void)parsed;
#endif
    return 0;
}

static int default_post_action(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes) {
    (void)ctx;
    if (data_len == 0U) {
        return 0;
    }
    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (data_len < NVME_POST_ACTION_RECORD_BYTES_SHORT) {
#if NVME_POST_ACTION_DEBUG
        fprintf(stderr,
                "post action debug: no 8-byte unit, offset=%llu data_len=%u\n",
                (unsigned long long)offset_bytes, (unsigned int)data_len);
#endif
        return 0;
    }

    const unsigned char *bytes = (const unsigned char *)data;
    // Fast path: decode each record with one/two 64-bit loads, then extract fields via bit ops.
    uint32_t cursor = 0U;
    uint32_t record_index = 0U;
    uint64_t local_invalid_records = 0ULL;
    nvme_post_action_time_ref_t time_ref;
    memset(&time_ref, 0, sizeof(time_ref));
    uint32_t parse_len = data_len;
    uint32_t tail_bytes = data_len % NVME_POST_ACTION_RECORD_BYTES_SHORT;
    if (tail_bytes != 0U) {
        // Keep parsing aligned records and treat tail fragment as one invalid item.
        parse_len -= tail_bytes;
        ++local_invalid_records;
    }

    while (cursor < parse_len) {
        const unsigned char *record = bytes + cursor;
        uint64_t record_offset = offset_bytes + (uint64_t)cursor;
        uint64_t record_lo = load_le64_u(record);
        uint8_t op = (uint8_t)(record_lo & 0xFFU);
        int rc = 0;

        // Termination marker: if both 8-byte units in a 16-byte window
        // start with 0x00, stop post-action parsing successfully.
        if ((data_len - cursor) >= NVME_POST_ACTION_RECORD_BYTES_LONG) {
            uint64_t record_hi_for_terminator = load_le64_u(record + 8U);
            uint8_t op_hi = (uint8_t)(record_hi_for_terminator & 0xFFU);
            if (op == 0x00U && op_hi == 0x00U) {
#if NVME_POST_ACTION_DEBUG
                fprintf(stderr,
                        "post action termination marker hit: offset=%llu record=%u\n",
                        (unsigned long long)record_offset,
                        (unsigned int)record_index);
#endif
                return 0;
            }
        }

        if (op == NVME_POST_ACTION_OP_READ || op == NVME_POST_ACTION_OP_WRITE) {
            if ((parse_len - cursor) < NVME_POST_ACTION_RECORD_BYTES_LONG) {
                // Invalid 16B record tail: count and stop to avoid crossing chunk boundary.
                ++local_invalid_records;
                break;
            }
            uint64_t record_hi = load_le64_u(record + 8U);
            rc = parse_rw_record(record_lo, record_hi, op, record_offset, record_index, &time_ref);
            cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
            ++record_index;
        } else if (op == NVME_POST_ACTION_OP_TRIM) {
            uint32_t consumed_bytes = 0U;
            uint32_t consumed_records = 0U;
            rc = parse_trim_group(bytes, parse_len, cursor, offset_bytes, record_index, &time_ref,
                                  &consumed_bytes, &consumed_records);
            if (rc != 0) {
                // Invalid trim group: count one bad record and skip current 16B record.
                ++local_invalid_records;
                cursor += NVME_POST_ACTION_RECORD_BYTES_LONG;
                ++record_index;
                continue;
            }
            cursor += consumed_bytes;
            record_index += consumed_records;
        } else if (op == NVME_POST_ACTION_OP_STAT) {
            rc = parse_stat_record(record_lo, record_offset, record_index, &time_ref);
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
        } else if (op == NVME_POST_ACTION_OP_MARKER) {
            rc = parse_marker_record(record_lo, record_offset, record_index, &time_ref);
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
        } else {
            ++local_invalid_records;
            cursor += NVME_POST_ACTION_RECORD_BYTES_SHORT;
            ++record_index;
            continue;
        }
        if (rc != 0) {
            // Parser-level invalid record: count and continue with next aligned record.
            ++local_invalid_records;
            continue;
        }
    }

#if NVME_POST_ACTION_DEBUG
    if (local_invalid_records > 0ULL) {
        fprintf(stderr,
                "post action invalid records: count=%llu chunk_offset=%llu\n",
                (unsigned long long)local_invalid_records,
                (unsigned long long)offset_bytes);
    }
#endif
    post_action_add_invalid_count(local_invalid_records);
    return 0;
}

int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx) {
    if (action == NULL) {
        g_post_action = default_post_action;
        g_post_action_ctx = NULL;
        return 0;
    }

    g_post_action = action;
    g_post_action_ctx = ctx;
    return 0;
}

int nvme_read_set_debug(int enabled) {
    g_nvme_read_debug = (enabled != 0) ? 1 : 0;
    return 0;
}

int nvme_post_action_process(void *data, uint32_t data_len, uint64_t offset_bytes) {
    if (g_post_action(g_post_action_ctx, data, data_len, offset_bytes) != 0) {
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static uint64_t get_mdts_chunk_bytes_or_default(int nvme_fd) {
    unsigned char *id_ctrl = NULL;
    if (posix_memalign((void **)&id_ctrl, 4096, 4096) != 0) {
        fprintf(stderr, "posix_memalign failed for identify buffer, fallback chunk=%llu\n",
                (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }
    memset(id_ctrl, 0, 4096);

    struct nvme_admin_cmd admin_cmd;
    memset(&admin_cmd, 0, sizeof(admin_cmd));
    admin_cmd.opcode = 0x06;  // Identify
    admin_cmd.nsid = 0;
    admin_cmd.addr = (uint64_t)(uintptr_t)id_ctrl;
    admin_cmd.data_len = 4096;
    admin_cmd.cdw10 = 1;      // CNS = 1, Identify Controller

    if (ioctl(nvme_fd, NVME_IOCTL_ADMIN_CMD, &admin_cmd) < 0) {
        fprintf(stderr, "identify controller failed: %s, fallback chunk=%llu\n",
                strerror(errno), (unsigned long long)NVME_READ_CHUNK_BYTES);
        free(id_ctrl);
        return NVME_READ_CHUNK_BYTES;
    }

    // Identify Controller data structure: byte 77 is MDTS.
    uint8_t mdts = id_ctrl[77];
    free(id_ctrl);

    if (mdts == 0U) {
        // 0 means no MDTS limit reported, keep using configured default chunk.
        fprintf(stderr, "mdts=0 (no limit reported), use fallback chunk=%llu\n",
                (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    if (mdts >= 52U) {
        fprintf(stderr, "mdts=%u too large, fallback chunk=%llu\n",
                (unsigned int)mdts, (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    uint64_t chunk_bytes = (1ULL << (12U + (uint64_t)mdts));
    if (chunk_bytes < NVME_LBA_SIZE_BYTES || (chunk_bytes % NVME_LBA_SIZE_BYTES) != 0ULL) {
        fprintf(stderr, "invalid mdts-derived chunk=%llu, fallback chunk=%llu\n",
                (unsigned long long)chunk_bytes, (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    if (chunk_bytes > (uint64_t)UINT32_MAX) {
        fprintf(stderr, "mdts-derived chunk too large=%llu, fallback chunk=%llu\n",
                (unsigned long long)chunk_bytes, (unsigned long long)NVME_READ_CHUNK_BYTES);
        return NVME_READ_CHUNK_BYTES;
    }

    fprintf(stderr, "mdts=%u, read chunk=%llu bytes\n",
            (unsigned int)mdts, (unsigned long long)chunk_bytes);
    return chunk_bytes;
}

static uint32_t get_sector_size_or_default(int nvme_fd) {
    int logical_block_size = 0;
    if (ioctl(nvme_fd, BLKSSZGET, &logical_block_size) != 0) {
        fprintf(stderr, "BLKSSZGET failed: %s, use default sector_size=%u\n",
                strerror(errno), (unsigned int)NVME_LBA_SIZE_BYTES);
        return (uint32_t)NVME_LBA_SIZE_BYTES;
    }

    if (logical_block_size <= 0) {
        fprintf(stderr, "invalid sector_size=%d, use default sector_size=%u\n",
                logical_block_size, (unsigned int)NVME_LBA_SIZE_BYTES);
        return (uint32_t)NVME_LBA_SIZE_BYTES;
    }

    uint32_t sector_size = (uint32_t)logical_block_size;
    fprintf(stderr, "detected sector_size=%u bytes\n", (unsigned int)sector_size);
    return sector_size;
}

int nvme_read(const char *device_name,
              uint64_t slba,
              uint64_t data_len,
              void *buffer) {
    (void)buffer;

    if (device_name == NULL || data_len == 0) {
        errno = EINVAL;
        fprintf(stderr, "invalid argument: device_name/data_len\n");
        return -1;
    }

    post_action_reset_invalid_count();

    int nvme_fd = open(device_name, O_RDONLY);
    if (nvme_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device_name, strerror(errno));
        return -1;
    }

    uint32_t sector_size = get_sector_size_or_default(nvme_fd);
    if (sector_size == 0U) {
        errno = EINVAL;
        fprintf(stderr, "invalid sector_size=0\n");
        close(nvme_fd);
        return -1;
    }

    if (data_len % (uint64_t)sector_size != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "data_len must be %u-byte aligned, got %llu\n",
                (unsigned int)sector_size, (unsigned long long)data_len);
        close(nvme_fd);
        return -1;
    }

    if (NVME_READ_CHUNK_BYTES % (uint64_t)sector_size != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "NVME_READ_CHUNK_BYTES must be %u-byte aligned\n",
                (unsigned int)sector_size);
        close(nvme_fd);
        return -1;
    }

    uint64_t read_chunk_bytes = get_mdts_chunk_bytes_or_default(nvme_fd);
    if ((read_chunk_bytes % (uint64_t)sector_size) != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "read chunk must be %u-byte aligned, got %llu\n",
                (unsigned int)sector_size, (unsigned long long)read_chunk_bytes);
        close(nvme_fd);
        return -1;
    }

    struct timespec ts_begin;
    if (clock_gettime(CLOCK_MONOTONIC, &ts_begin) != 0) {
        fprintf(stderr, "clock_gettime begin failed: %s\n", strerror(errno));
        close(nvme_fd);
        return -1;
    }

    uint64_t total_read_bytes = 0ULL;
    if (run_read_post_pipeline(nvme_fd, sector_size, slba, data_len, read_chunk_bytes,
                               &total_read_bytes) != 0) {
        close(nvme_fd);
        return -1;
    }

    struct timespec ts_end;
    if (g_nvme_read_debug != 0 && clock_gettime(CLOCK_MONOTONIC, &ts_end) == 0) {
        double elapsed_s = (double)(ts_end.tv_sec - ts_begin.tv_sec) +
                           (double)(ts_end.tv_nsec - ts_begin.tv_nsec) / 1000000000.0;
        if (elapsed_s <= 0.0) {
            elapsed_s = 1e-9;
        }
        double bandwidth_mib_s =
            ((double)total_read_bytes / (1024.0 * 1024.0)) / elapsed_s;
        uint64_t invalid_records = post_action_get_invalid_count();
        fprintf(stderr,
                "read stats: bytes=%llu elapsed=%.6f sec bandwidth=%.2f MiB/s invalid_records=%llu\n",
                (unsigned long long)total_read_bytes,
                elapsed_s,
                bandwidth_mib_s,
                (unsigned long long)invalid_records);
    }

    close(nvme_fd);
    return 0;
}
