#ifndef INSIGHT_API_H
#define INSIGHT_API_H

#include <stdint.h>

#include "insight_metalog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGHT_JSON_BUFFER_BYTES 65536U

/**
 * All queries resolve the time window and LBA filter from @p session_id via
 * insight_metalog_read() and insight_metalog_session_time_window(). Post-action
 * filtering uses the session lba_map bitmap (4KiB units) only.
 */
int get_read_latency_percentiles(const char *device,
                                 int64_t session_id,
                                 char *json_buffer);

int get_write_latency_percentiles(const char *device,
                                  int64_t session_id,
                                  char *json_buffer);

int get_write_amplification(const char *device,
                            int64_t session_id,
                            char *json_buffer);

int get_qd_distribution(const char *device,
                        int64_t session_id,
                        char *json_buffer);

int get_read_size_distribution(const char *device,
                               int64_t session_id,
                               char *json_buffer);

int get_write_size_distribution(const char *device,
                                int64_t session_id,
                                char *json_buffer);

int get_read_throughput_distribution(const char *device,
                                     int64_t session_id,
                                     char *json_buffer);

int get_write_throughput_distribution(const char *device,
                                      int64_t session_id,
                                      char *json_buffer);

int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                int64_t session_id,
                                char *json_buffer);

int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         int64_t session_id,
                                         char *json_buffer);

int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               int64_t session_id,
                               char *json_buffer);

int get_nand_write_volume(const char *device,
                          int64_t session_id,
                          char *json_buffer);

int get_gc_data_movement(const char *device,
                         int64_t session_id,
                         char *json_buffer);

#ifdef __cplusplus
}
#endif

#endif  // INSIGHT_API_H
