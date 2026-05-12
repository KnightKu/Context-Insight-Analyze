#ifndef NVME_READ_H
#define NVME_READ_H

#include <stdint.h>

#define NVME_READ_CHUNK_BYTES (256ULL * 1024ULL)
#define NVME_SPLIT_BYTES (10ULL * 1024ULL * 1024ULL * 1024ULL)
#define NVME_DEFAULT_DATA_LEN NVME_SPLIT_BYTES
#define NVME_LBA_SIZE_BYTES 4096ULL
#define LOG_START_LBA (963899008ULL * NVME_LBA_SIZE_BYTES)
#define LOG_END_LBA (3516309888ULL * NVME_LBA_SIZE_BYTES)

#ifndef INSIGHT_PERF_DEBUG
#define INSIGHT_PERF_DEBUG 0
#endif

typedef int (*nvme_read_post_action_t)(void *ctx,
                                       void *data,
                                       uint32_t data_len,
                                       uint64_t offset_bytes);

typedef struct {
    uint64_t io_ns;
    uint64_t post_action_ns;
    uint64_t io_calls;
    uint64_t post_action_calls;
    uint64_t pipeline_total_ns;
} nvme_read_perf_stats_t;

int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx);

int nvme_read_set_debug(int enabled);

int nvme_read_set_latency(int enabled);

int nvme_read_set_lba_stats_read_count(int enabled);

int nvme_read_set_lba_stats_w2fr(int enabled);

int nvme_read_set_lba_stats_life_cycle(int enabled);

int nvme_read_set_qd_dist(int enabled);

int nvme_read_set_wa_dist(int enabled);

int nvme_read_set_read_size_dist(int enabled);

int nvme_read_set_write_size_dist(int enabled);

int nvme_read_set_trim_size_dist(int enabled);

int nvme_read_set_read_throughput_dist(int enabled);

int nvme_read_set_write_throughput_dist(int enabled);

int nvme_read_set_format_json(int enabled);

int nvme_read_set_time_window(int has_start, uint64_t time_start_ms,
                              int has_end, uint64_t time_end_ms);

int nvme_read_set_block_size_bytes(uint64_t block_size_bytes);

int nvme_read_set_stat_sample_collection(int enabled);

/**
 * When non-zero (default), nvme_read prints optional end-of-run output:
 * latency/LBA/workload summaries; with -D/--debug, read bandwidth stats; and
 * when built with NVME_POST_ACTION_PERF_DEBUG, the post-action perf summary.
 */
int nvme_read_set_print_end_reports(int enabled);

int nvme_read_get_print_end_reports(void);

int nvme_post_action_process(void *data,
                             uint32_t data_len,
                             uint64_t offset_bytes);

int nvme_post_action_set_sector_size(uint32_t sector_size);

int nvme_read(const char *device_name,
              uint64_t slba,
              uint64_t data_len,
              void *buffer);

void nvme_read_perf_reset(void);

void nvme_read_perf_get(nvme_read_perf_stats_t *out);

#endif  // NVME_READ_H
