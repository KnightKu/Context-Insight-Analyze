#ifndef INSIGHT_API_H
#define INSIGHT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGHT_JSON_BUFFER_BYTES 65536U

int get_read_latency_percentiles(const char *device,
                                 uint64_t block_size,
                                 const char *time_start,
                                 const char *time_end,
                                 char *json_buffer);

int get_write_amplification(const char *device,
                            uint64_t block_size,
                            const char *time_start,
                            const char *time_end,
                            char *json_buffer);

int get_qd_distribution(const char *device,
                        uint64_t block_size,
                        const char *time_start,
                        const char *time_end,
                        char *json_buffer);

#ifdef __cplusplus
}
#endif

#endif  // INSIGHT_API_H
