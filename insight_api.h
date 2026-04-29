#ifndef INSIGHT_API_H
#define INSIGHT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGHT_JSON_BUFFER_BYTES 65536U

int get_read_latency_percentiles(const char *device,
                                 const char *time_start,
                                 const char *time_end,
                                 char *json_buffer);

int get_write_amplification(const char *device,
                            const char *time_start,
                            const char *time_end,
                            char *json_buffer);

int get_qd_distribution(const char *device,
                        const char *time_start,
                        const char *time_end,
                        char *json_buffer);

int get_read_size_distribution(const char *device,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer);

int get_write_size_distribution(const char *device,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer);

int get_read_throughput_distribution(const char *device,
                                    const char *time_start,
                                    const char *time_end,
                                    char *json_buffer);

int get_write_throughput_distribution(const char *device,
                                     const char *time_start,
                                     const char *time_end,
                                     char *json_buffer);

int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer);

int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         char *json_buffer);

int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer);

int get_nand_write_volume(const char *device,
                          const char *time_start,
                          const char *time_end,
                          char *json_buffer);

int get_gc_data_movement(const char *device,
                         const char *time_start,
                         const char *time_end,
                         char *json_buffer);

#ifdef __cplusplus
}
#endif

#endif  // INSIGHT_API_H
