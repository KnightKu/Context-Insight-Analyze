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

typedef struct {
    uint64_t abs_time_us;
    uint16_t qd;
    uint32_t hot_write_4k;
    uint32_t folding_write_4k;
} nvme_post_action_stat_sample_t;

int nvme_post_action_stats_init(uint32_t sector_size);

int nvme_post_action_stats_set_mdts_bytes(uint64_t mdts_bytes);

void nvme_post_action_stats_advanced_record_overwrite(uint64_t start_lba,
                                                      uint64_t len_lba,
                                                      uint64_t abs_time_us);

int nvme_post_action_stats_get_advanced_max_range_kib(uint64_t *max_range_kib_out);

int nvme_post_action_stats_get_advanced_life_count(uint64_t range_kib,
                                                   uint16_t life_code,
                                                   uint64_t *count_out);

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

int nvme_post_action_stats_get_advanced_max_scale(uint32_t *max_scale_out);

int nvme_post_action_stats_get_advanced_count(uint32_t scale_idx,
                                              uint16_t life_code,
                                              uint64_t *count_out);

int nvme_post_action_stats_record_stat(uint64_t abs_time_us,
                                       uint16_t qd,
                                       uint32_t hot_write_4k,
                                       uint32_t folding_write_4k);

int nvme_post_action_stats_get_stat_sample_count(uint64_t *count_out);

int nvme_post_action_stats_get_stat_sample(uint64_t sample_index,
                                           nvme_post_action_stat_sample_t *out);

void nvme_post_action_stats_reset_stat_samples(void);

void nvme_post_action_stats_print_ratio_summary(int print_read_count,
                                                int print_w2fr,
                                                int print_life_cycle);

void nvme_post_action_stats_set_json_format_enabled(int enabled);

int nvme_post_action_stats_get_json_format_enabled(void);

void nvme_post_action_stats_print_read_count_ratio_summary(void);

void nvme_post_action_stats_print_w2fr_ratio_summary(void);

void nvme_post_action_stats_print_life_cycle_ratio_summary(void);

#endif  // POST_ACTION_STATS_H
