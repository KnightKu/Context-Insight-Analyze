#ifndef POST_ACTION_LATENCY_H
#define POST_ACTION_LATENCY_H

#include <stdint.h>

void nvme_post_action_latency_set_enabled(int enabled);

int nvme_post_action_latency_get_enabled(void);

void nvme_post_action_latency_set_json_format(int enabled);

void nvme_post_action_latency_record_read(uint32_t latency_us);

void nvme_post_action_latency_record_write(uint32_t latency_us);

void nvme_post_action_latency_record_trim(uint32_t latency_us);

void nvme_post_action_latency_reset(void);

void nvme_post_action_latency_print_summary(void);

#endif  // POST_ACTION_LATENCY_H
