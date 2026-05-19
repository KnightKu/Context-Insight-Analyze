#ifndef INSIGHT_API_H
#define INSIGHT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGHT_JSON_BUFFER_BYTES 65536U

/** Pass to insight_api_set_json_query_session_id() to emit time_start/time_end in query JSON. */
#define INSIGHT_JSON_QUERY_SESSION_ID_NONE (-1LL)

/**
 * When set to a value >= 0, compose_query_result_json() puts session_id in the
 * query object and omits time_start/time_end. When INSIGHT_JSON_QUERY_SESSION_ID_NONE,
 * query JSON uses time_start/time_end and omits session_id.
 */
int insight_api_set_json_query_session_id(int64_t session_id);

int get_read_latency_percentiles(const char *device,
                                 const char *time_start,
                                 const char *time_end,
                                 uint64_t lba_start,
                                 uint64_t lba_end,
                                 char *json_buffer);

int get_write_latency_percentiles(const char *device,
                                  const char *time_start,
                                  const char *time_end,
                                  uint64_t lba_start,
                                  uint64_t lba_end,
                                  char *json_buffer);

int get_write_amplification(const char *device,
                            const char *time_start,
                            const char *time_end,
                            uint64_t lba_start,
                            uint64_t lba_end,
                            char *json_buffer);

int get_qd_distribution(const char *device,
                        const char *time_start,
                        const char *time_end,
                        uint64_t lba_start,
                        uint64_t lba_end,
                        char *json_buffer);

int get_read_size_distribution(const char *device,
                               const char *time_start,
                               const char *time_end,
                               uint64_t lba_start,
                               uint64_t lba_end,
                               char *json_buffer);

int get_write_size_distribution(const char *device,
                                const char *time_start,
                                const char *time_end,
                                uint64_t lba_start,
                                uint64_t lba_end,
                                char *json_buffer);

int get_read_throughput_distribution(const char *device,
                                    const char *time_start,
                                    const char *time_end,
                                    uint64_t lba_start,
                                    uint64_t lba_end,
                                    char *json_buffer);

int get_write_throughput_distribution(const char *device,
                                     const char *time_start,
                                     const char *time_end,
                                     uint64_t lba_start,
                                     uint64_t lba_end,
                                     char *json_buffer);

int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                uint64_t lba_start,
                                uint64_t lba_end,
                                char *json_buffer);

int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         uint64_t lba_start,
                                         uint64_t lba_end,
                                         char *json_buffer);

int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               uint64_t lba_start,
                               uint64_t lba_end,
                               char *json_buffer);

int get_nand_write_volume(const char *device,
                          const char *time_start,
                          const char *time_end,
                          uint64_t lba_start,
                          uint64_t lba_end,
                          char *json_buffer);

int get_gc_data_movement(const char *device,
                         const char *time_start,
                         const char *time_end,
                         uint64_t lba_start,
                         uint64_t lba_end,
                         char *json_buffer);

#ifdef __cplusplus
}
#endif

#endif  // INSIGHT_API_H
