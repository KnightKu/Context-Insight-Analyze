#ifndef POST_ACTION_STATS_H
#define POST_ACTION_STATS_H

#include <stdint.h>

#define NVME_POST_ACTION_STATS_ENTRY_BYTES 5U
#define NVME_POST_ACTION_STATS_BLOCK_BYTES 4096ULL
#define NVME_POST_ACTION_DEFAULT_TOTAL_LBA_BYTES (4ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define NVME_POST_ACTION_DEFAULT_SECTOR_SIZE 512U
#define NVME_POST_ACTION_LATENCY_CODE_MAX 65535U

typedef struct {
    uint8_t read_count;
    uint16_t write_to_first_read_latency;
    uint16_t life_cycle_latency;
} __attribute__((packed)) nvme_post_action_lba_stat_t;

_Static_assert(sizeof(nvme_post_action_lba_stat_t) == NVME_POST_ACTION_STATS_ENTRY_BYTES,
               "nvme_post_action_lba_stat_t must be 5 bytes");

int nvme_post_action_stats_init(uint32_t sector_size);

int nvme_post_action_stats_set_mdts_bytes(uint64_t mdts_bytes);

void nvme_post_action_stats_advanced_record_overwrite(uint64_t start_lba,
                                                      uint64_t len_lba,
                                                      uint64_t abs_time_us);

void nvme_post_action_stats_update_write(uint64_t start_lba,
                                         uint64_t len_lba,
                                         uint64_t abs_time_us);

void nvme_post_action_stats_update_read(uint64_t start_lba,
                                        uint64_t len_lba,
                                        uint64_t abs_time_us);

void nvme_post_action_stats_print_summary_debug(int debug_enabled);

int nvme_post_action_stats_get_bucket_count(uint64_t *bucket_count_out);

int nvme_post_action_stats_get_bucket(uint64_t bucket_index,
                                      nvme_post_action_lba_stat_t *out);

#endif  // POST_ACTION_STATS_H
