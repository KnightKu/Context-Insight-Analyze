#ifndef INSIGHT_API_JSON_H
#define INSIGHT_API_JSON_H

#include <stdint.h>
#include <stdio.h>

#include "insight_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGHT_JSON_CAPTURE_MAX_BYTES (INSIGHT_JSON_BUFFER_BYTES * 4U)

void insight_json_skip_ws(const char **p);

/**
 * Parse a JSON object starting at @p start (must point to '{').
 * On success, *end_out is set to the position immediately after the closing '}'.
 */
int insight_json_match_object(const char *start, const char **end_out);

/**
 * Copy the last complete root JSON object in @p text into @p out.
 * Ignores earlier non-JSON noise and incomplete brace spans.
 */
int insight_json_extract_last_root_object(const char *text, char *out, size_t out_size);

/**
 * Read up to @p max_read_bytes from @p src and extract the last root JSON object.
 */
int insight_json_capture_stderr_object(FILE *src, char *out, size_t out_size, size_t max_read_bytes);

int insight_json_append_escaped_string(char *dst, size_t dst_size, size_t *cursor, const char *src);

int insight_json_compose_query_result(const char *api_name,
                                      const char *device,
                                      int include_block_size,
                                      uint64_t block_size,
                                      const char *time_start,
                                      const char *time_end,
                                      int64_t session_id,
                                      const char *result_json,
                                      char *json_buffer);

int insight_json_flatten_single_root_object(const char *input_json,
                                            char *flattened_json,
                                            size_t flattened_json_size);

/** Returns 0 when @p json is a single root object with only trailing whitespace after. */
int insight_json_validate_root_object(const char *json);

#ifdef __cplusplus
}
#endif

#endif /* INSIGHT_API_JSON_H */
