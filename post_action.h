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

void nvme_post_action_reset_latency_stats(void);

void nvme_post_action_print_latency_report(void);

#endif  // POST_ACTION_H
