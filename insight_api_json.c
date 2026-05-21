#define _POSIX_C_SOURCE 200809L

#include "insight_api_json.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int json_scan_string(const char **p) {
    if (p == NULL || *p == NULL || **p != '\"') {
        return -1;
    }
    ++(*p);
    while (**p != '\0') {
        if (**p == '\\' && (*p)[1] != '\0') {
            *p += 2;
            continue;
        }
        if (**p == '\"') {
            ++(*p);
            return 0;
        }
        ++(*p);
    }
    return -1;
}

void insight_json_skip_ws(const char **p) {
    if (p == NULL || *p == NULL) {
        return;
    }
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        ++(*p);
    }
}

int insight_json_match_object(const char *start, const char **end_out) {
    if (start == NULL || end_out == NULL || *start != '{') {
        errno = EINVAL;
        return -1;
    }
    const char *p = start;
    int depth = 0;
    while (*p != '\0') {
        if (*p == '\"') {
            if (json_scan_string(&p) != 0) {
                errno = EINVAL;
                return -1;
            }
            continue;
        }
        if (*p == '{') {
            ++depth;
            ++p;
            continue;
        }
        if (*p == '}') {
            --depth;
            if (depth == 0) {
                ++p;
                *end_out = p;
                return 0;
            }
            ++p;
            continue;
        }
        ++p;
    }
    errno = EINVAL;
    return -1;
}

int insight_json_validate_root_object(const char *json) {
    if (json == NULL) {
        errno = EINVAL;
        return -1;
    }
    const char *p = json;
    insight_json_skip_ws(&p);
    if (*p != '{') {
        errno = EINVAL;
        return -1;
    }
    const char *end = NULL;
    if (insight_json_match_object(p, &end) != 0) {
        return -1;
    }
    insight_json_skip_ws(&end);
    if (*end != '\0') {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int insight_json_extract_last_root_object(const char *text, char *out, size_t out_size) {
    if (text == NULL || out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    const char *best_start = NULL;
    const char *best_end = NULL;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p != '{') {
            continue;
        }
        const char *end = NULL;
        if (insight_json_match_object(p, &end) != 0) {
            continue;
        }
        const char *tail = end;
        insight_json_skip_ws(&tail);
        if (*tail != '\0') {
            continue;
        }
        best_start = p;
        best_end = end;
    }

    if (best_start == NULL || best_end == NULL || best_end <= best_start) {
        errno = ENODATA;
        return -1;
    }

    size_t json_len = (size_t)(best_end - best_start);
    if (json_len >= out_size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(out, best_start, json_len);
    out[json_len] = '\0';
    return 0;
}

int insight_json_capture_stderr_object(FILE *src, char *out, size_t out_size, size_t max_read_bytes) {
    if (src == NULL || out == NULL || out_size == 0U || max_read_bytes == 0U) {
        errno = EINVAL;
        return -1;
    }

    char *scratch = (char *)malloc(max_read_bytes);
    if (scratch == NULL) {
        errno = ENOMEM;
        return -1;
    }

    size_t total = 0U;
    while (total < (max_read_bytes - 1U)) {
        size_t room = (max_read_bytes - 1U) - total;
        size_t got = fread(scratch + total, 1U, room, src);
        if (got == 0U) {
            break;
        }
        total += got;
    }
    scratch[total] = '\0';

    int rc = insight_json_extract_last_root_object(scratch, out, out_size);
    free(scratch);
    return rc;
}

int insight_json_append_escaped_string(char *dst, size_t dst_size, size_t *cursor, const char *src) {
    if (dst == NULL || cursor == NULL || src == NULL || dst_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        const char *esc = NULL;
        switch (*p) {
            case '\"':
                esc = "\\\"";
                break;
            case '\\':
                esc = "\\\\";
                break;
            case '\b':
                esc = "\\b";
                break;
            case '\f':
                esc = "\\f";
                break;
            case '\n':
                esc = "\\n";
                break;
            case '\r':
                esc = "\\r";
                break;
            case '\t':
                esc = "\\t";
                break;
            default:
                break;
        }
        if (esc != NULL) {
            size_t n = strlen(esc);
            if ((*cursor + n) >= dst_size) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(dst + *cursor, esc, n);
            *cursor += n;
            continue;
        }
        if (*p < 0x20U) {
            if ((*cursor + 6U) >= dst_size) {
                errno = ENOSPC;
                return -1;
            }
            int n = snprintf(dst + *cursor, dst_size - *cursor, "\\u%04x", (unsigned int)*p);
            if (n < 0 || (size_t)n >= (dst_size - *cursor)) {
                errno = ENOSPC;
                return -1;
            }
            *cursor += (size_t)n;
            continue;
        }
        if ((*cursor + 1U) >= dst_size) {
            errno = ENOSPC;
            return -1;
        }
        dst[*cursor] = (char)*p;
        *cursor += 1U;
    }
    return 0;
}

int insight_json_compose_query_result(const char *api_name,
                                      const char *device,
                                      int include_block_size,
                                      uint64_t block_size,
                                      const char *time_start,
                                      const char *time_end,
                                      int64_t session_id,
                                      const char *result_json,
                                      char *json_buffer) {
    if (api_name == NULL || device == NULL || result_json == NULL || json_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (session_id < 0 || time_start == NULL || time_end == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (insight_json_validate_root_object(result_json) != 0) {
        errno = EINVAL;
        return -1;
    }

    size_t pos = 0U;
    int n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                     "{\n"
                     "  \"query\": {\n"
                     "    \"api\": \"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (insight_json_append_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, api_name) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "\",\n"
                 "    \"device\": \"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (insight_json_append_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, device) != 0) {
        return -1;
    }
    if (include_block_size != 0) {
        n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                     "\",\n"
                     "    \"block_size\": %llu",
                     (unsigned long long)block_size);
        if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
            errno = ENOSPC;
            return -1;
        }
        pos += (size_t)n;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "%s\n"
                 "    \"session_id\": %" PRIu64 ",\n"
                 "    \"time_start\": \"",
                 (include_block_size != 0) ? "," : "\",",
                 (uint64_t)session_id);
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (insight_json_append_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, time_start) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "\",\n"
                 "    \"time_end\": \"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    if (insight_json_append_escaped_string(json_buffer, INSIGHT_JSON_BUFFER_BYTES, &pos, time_end) != 0) {
        return -1;
    }
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos, "\"");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    n = snprintf(json_buffer + pos, INSIGHT_JSON_BUFFER_BYTES - pos,
                 "\n"
                 "  },\n"
                 "  \"result\": ");
    if (n < 0 || (size_t)n >= (INSIGHT_JSON_BUFFER_BYTES - pos)) {
        errno = ENOSPC;
        return -1;
    }
    pos += (size_t)n;
    size_t result_len = strlen(result_json);
    if ((pos + result_len + 3U) >= INSIGHT_JSON_BUFFER_BYTES) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(json_buffer + pos, result_json, result_len);
    pos += result_len;
    json_buffer[pos++] = '\n';
    json_buffer[pos++] = '}';
    json_buffer[pos] = '\0';
    return 0;
}

int insight_json_flatten_single_root_object(const char *input_json,
                                            char *flattened_json,
                                            size_t flattened_json_size) {
    if (input_json == NULL || flattened_json == NULL || flattened_json_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    const char *p = input_json;
    insight_json_skip_ws(&p);
    if (*p != '{') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    insight_json_skip_ws(&p);
    if (*p != '\"') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
            continue;
        }
        if (*p == '\"') {
            ++p;
            break;
        }
        ++p;
    }
    if (*(p - 1) != '\"') {
        errno = EINVAL;
        return -1;
    }
    insight_json_skip_ws(&p);
    if (*p != ':') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    insight_json_skip_ws(&p);
    if (*p != '{') {
        errno = EINVAL;
        return -1;
    }
    const char *value_start = p;
    int depth = 0;
    while (*p != '\0') {
        if (*p == '\"') {
            ++p;
            while (*p != '\0') {
                if (*p == '\\' && p[1] != '\0') {
                    p += 2;
                    continue;
                }
                if (*p == '\"') {
                    ++p;
                    break;
                }
                ++p;
            }
            continue;
        }
        if (*p == '{') {
            ++depth;
        } else if (*p == '}') {
            --depth;
            if (depth == 0) {
                ++p;
                break;
            }
        }
        ++p;
    }
    if (depth != 0) {
        errno = EINVAL;
        return -1;
    }
    const char *value_end = p;
    insight_json_skip_ws(&p);
    if (*p != '}') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    insight_json_skip_ws(&p);
    if (*p != '\0') {
        errno = EINVAL;
        return -1;
    }
    size_t out_len = (size_t)(value_end - value_start);
    if (out_len >= flattened_json_size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(flattened_json, value_start, out_len);
    flattened_json[out_len] = '\0';
    return 0;
}
