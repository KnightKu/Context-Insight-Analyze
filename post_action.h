#ifndef POST_ACTION_H
#define POST_ACTION_H

#include "nvme_read.h"

#include <stdint.h>

void nvme_post_action_reset_invalid_count(void);

uint64_t nvme_post_action_get_invalid_count(void);

int nvme_post_action_set_sector_size(uint32_t sector_size);

int nvme_post_action_set_base_lba(uint64_t base_lba);

int nvme_post_action_default(void *ctx, void *data, uint32_t data_len, uint64_t offset_bytes);

int nvme_post_action_set_handler(nvme_read_post_action_t action, void *ctx);

int nvme_post_action_process(void *data, uint32_t data_len, uint64_t offset_bytes);

int nvme_post_action_set_debug(int enabled);

int nvme_post_action_get_debug(void);

int nvme_post_action_set_latency_enabled(int enabled);

int nvme_post_action_get_latency_enabled(void);

int nvme_post_action_set_json_format_enabled(int enabled);

int nvme_post_action_get_json_format_enabled(void);

int nvme_post_action_set_time_window_ms(int enabled, uint64_t start_ms, uint64_t end_ms);

int nvme_post_action_get_time_window_ms(int *enabled, uint64_t *start_ms, uint64_t *end_ms);

int nvme_post_action_set_block_size_bytes(uint64_t block_size_bytes);

uint64_t nvme_post_action_get_block_size_bytes(void);

void nvme_post_action_reset_latency_stats(void);

void nvme_post_action_print_latency_report(void);

int nvme_post_action_set_lba_read_count_enabled(int enabled);

int nvme_post_action_get_lba_read_count_enabled(void);

int nvme_post_action_set_lba_w2fr_enabled(int enabled);

int nvme_post_action_get_lba_w2fr_enabled(void);

int nvme_post_action_set_lba_life_cycle_enabled(int enabled);

int nvme_post_action_get_lba_life_cycle_enabled(void);

int nvme_post_action_set_qd_dist_enabled(int enabled);

int nvme_post_action_get_qd_dist_enabled(void);

int nvme_post_action_set_wa_dist_enabled(int enabled);

int nvme_post_action_get_wa_dist_enabled(void);

int nvme_post_action_set_read_size_dist_enabled(int enabled);

int nvme_post_action_get_read_size_dist_enabled(void);

int nvme_post_action_set_write_size_dist_enabled(int enabled);

int nvme_post_action_get_write_size_dist_enabled(void);

int nvme_post_action_set_trim_size_dist_enabled(int enabled);

int nvme_post_action_get_trim_size_dist_enabled(void);

void nvme_post_action_print_lba_stats_report(void);

void nvme_post_action_print_workload_stats_report(void);

#endif  // POST_ACTION_H
