#ifndef INSIGHT_API_H
#define INSIGHT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGHT_JSON_BUFFER_BYTES 65536U

/**
 * Use for @p session_id to select caller-supplied time_start/time_end and lba_start/lba_end.
 * Any other value selects session mode: insight_api resolves time/LBA via insight_metalog_read()
 * and insight_get_session_time_window() (caller time/LBA may be NULL / ignored).
 */
#define INSIGHT_JSON_QUERY_SESSION_ID_NONE (-1LL)

int get_read_latency_percentiles(const char *device,
                                 const char *time_start,
                                 const char *time_end,
                                 uint64_t lba_start,
                                 uint64_t lba_end,
                                 int64_t session_id,
                                 char *json_buffer);

int get_write_latency_percentiles(const char *device,
                                  const char *time_start,
                                  const char *time_end,
                                  uint64_t lba_start,
                                  uint64_t lba_end,
                                  int64_t session_id,
                                  char *json_buffer);

int get_write_amplification(const char *device,
                            const char *time_start,
                            const char *time_end,
                            uint64_t lba_start,
                            uint64_t lba_end,
                            int64_t session_id,
                            char *json_buffer);

int get_qd_distribution(const char *device,
                        const char *time_start,
                        const char *time_end,
                        uint64_t lba_start,
                        uint64_t lba_end,
                        int64_t session_id,
                        char *json_buffer);

int get_read_size_distribution(const char *device,
                               const char *time_start,
                               const char *time_end,
                               uint64_t lba_start,
                               uint64_t lba_end,
                               int64_t session_id,
                               char *json_buffer);

int get_write_size_distribution(const char *device,
                                const char *time_start,
                                const char *time_end,
                                uint64_t lba_start,
                                uint64_t lba_end,
                                int64_t session_id,
                                char *json_buffer);

int get_read_throughput_distribution(const char *device,
                                     const char *time_start,
                                     const char *time_end,
                                     uint64_t lba_start,
                                     uint64_t lba_end,
                                     int64_t session_id,
                                     char *json_buffer);

int get_write_throughput_distribution(const char *device,
                                      const char *time_start,
                                      const char *time_end,
                                      uint64_t lba_start,
                                      uint64_t lba_end,
                                      int64_t session_id,
                                      char *json_buffer);

int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                uint64_t lba_start,
                                uint64_t lba_end,
                                int64_t session_id,
                                char *json_buffer);

int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         uint64_t lba_start,
                                         uint64_t lba_end,
                                         int64_t session_id,
                                         char *json_buffer);

int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               uint64_t lba_start,
                               uint64_t lba_end,
                               int64_t session_id,
                               char *json_buffer);

int get_nand_write_volume(const char *device,
                          const char *time_start,
                          const char *time_end,
                          uint64_t lba_start,
                          uint64_t lba_end,
                          int64_t session_id,
                          char *json_buffer);

int get_gc_data_movement(const char *device,
                         const char *time_start,
                         const char *time_end,
                         uint64_t lba_start,
                         uint64_t lba_end,
                         int64_t session_id,
                         char *json_buffer);

#ifdef __cplusplus
}
#endif

#endif  // INSIGHT_API_H
