#ifndef POST_ACTION_EXPORT_H
#define POST_ACTION_EXPORT_H

#include <stdint.h>

int nvme_post_action_export_stats_csv(const char *csv_path,
                                      uint64_t start_bucket,
                                      uint64_t bucket_count);

int nvme_post_action_export_advanced_life_cycle_csv(const char *csv_path);

int nvme_post_action_export_stat_qd_csv(const char *csv_path);

int nvme_post_action_export_stat_wa_csv(const char *csv_path);

#endif  // POST_ACTION_EXPORT_H
