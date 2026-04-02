#define _POSIX_C_SOURCE 200809L
#include "nvme_read.h"
#include "post_action.h"
#include "post_action_export.h"
#include "post_action_latency.h"
#include "post_action_stats.h"

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

#define NVME_READ_PIPELINE_SLOTS 4U

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

static int g_nvme_read_debug = 0;

int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx) {
    return nvme_post_action_set_handler(action, ctx);
}

int nvme_read_set_debug(int enabled) {
    g_nvme_read_debug = (enabled != 0) ? 1 : 0;
    return nvme_post_action_set_debug(g_nvme_read_debug);
}

int nvme_read_set_latency(int enabled) {
    nvme_post_action_latency_set_enabled(enabled);
    return 0;
}

int nvme_read_set_mdts_bytes(uint64_t mdts_bytes) {
    return nvme_post_action_stats_set_mdts_bytes(mdts_bytes);
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

    nvme_post_action_reset_invalid_count();

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
    (void)nvme_post_action_set_sector_size(sector_size);
    (void)nvme_post_action_set_base_lba(slba);

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
    (void)nvme_read_set_mdts_bytes(read_chunk_bytes);
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
        uint64_t invalid_records = nvme_post_action_get_invalid_count();
        fprintf(stderr,
                "read stats: bytes=%llu elapsed=%.6f sec bandwidth=%.2f MiB/s invalid_records=%llu\n",
                (unsigned long long)total_read_bytes,
                elapsed_s,
                bandwidth_mib_s,
                (unsigned long long)invalid_records);
        nvme_post_action_stats_print_summary_debug(g_nvme_read_debug);
        nvme_post_action_latency_print_summary();
    }

    close(nvme_fd);
    return 0;
}
