#ifndef POST_ACTION_EXPORT_H
#define POST_ACTION_EXPORT_H

#include <stdint.h>

int nvme_post_action_export_stats_csv(const char *csv_path,
                                      uint64_t start_bucket,
                                      uint64_t bucket_count);

#endif  // POST_ACTION_EXPORT_H
