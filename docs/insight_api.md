# Insight API Reference (`insight_api.h`)

This document is the authoritative API reference for all public interfaces in
`insight_api.h`.

## 1. Build and Link

Build from repository root:

```bash
make clean && make
```

Include and link:

- Header: `include/insight_api.h` (compile with `-Iinclude`)
- Static library: `libinsight_api.a`
- Shared library: `libinsight_api.so`
- Linker flags: `-linsight_api -lpthread -lm`

### 1.1 Build shared library and examples

```bash
make shared examples
```

### 1.2 Example programs

- `examples/insight_api_example.c` — time-window queries (`time_start` / `time_end`)
- `examples/insight_api_session_example.c` — `session_id` mode (metalog resolves time/LBA)

```bash
make examples
sudo ./insight_api_example /dev/nvme0n1 "2026-04-26 10:05:05" "2026-04-26 12:10:05" 4096
sudo ./insight_api_session_example /dev/nvme0n1 42 4096
```

## 2. Common Contract

Most APIs use the following common parameter pattern:

- `device`: NVMe device path (for example `/dev/nvme0n1`), must not be `NULL`
- `time_start` / `time_end`: format `YYYY-MM-DD HH:MM:SS`, and `time_start <= time_end`
- `json_buffer`: caller-provided output buffer, must not be `NULL`
  - recommended size: `INSIGHT_JSON_BUFFER_BYTES` (65536)
- For APIs that include `block_size`, it is in bytes and must be positive.

Return value:

- `0`: success, JSON string is written into `json_buffer`
- `-1`: failure, `errno` indicates reason

Typical `errno`:

- `EINVAL`: invalid arguments or invalid time format/range
- `ERANGE`: invalid scan range
- `ENODATA`: no JSON payload captured
- `ENOSPC`: output buffer too small
- `ENOMEM`: memory allocation failure
- `EIO`: I/O or internal pipeline error

Thread safety:

- APIs are protected by an internal global mutex, so concurrent callers are serialized.

Compile-time profiling switch (for latency performance analysis):

- Define macro `INSIGHT_API_PERF_DEBUG=1` when building to enable detailed stage timing logs.
- Example:
  - `make clean && make CFLAGS="-Wall -Wextra -pedantic -std=c11 -Iinclude -Isrc -DINSIGHT_API_PERF_DEBUG=1 -DINSIGHT_PERF_DEBUG=1"`
- With profiling enabled, `get_read_latency_percentiles` path prints:
  - API stage time (`config/reset/stderr_redirect/nvme_read/capture/restore`)
  - `nvme_read` internal time (`io`, `post_action`, `pipeline_total`) and call counts
  - Latency bucket extraction time (`extract_total`)

Output envelope note:

- All APIs return an extended envelope:
  - `query`: function name and input parameters
  - `result`: API result payload

---

## 3. API Details

### 3.1 `get_read_latency_percentiles`

```c
int get_read_latency_percentiles(const char *device,
                                 const char *time_start,
                                 const char *time_end,
                                 char *json_buffer);
```

Function:

- Query read latency percentile statistics only.

Top-level JSON objects:

- `query`
- `result`

Example JSON shape:

```json
{
  "query": {
    "api": "get_read_latency_percentiles",
    "device": "/dev/nvme0n1",
    "time_start": "2026-04-26 10:05:05",
    "time_end": "2026-04-26 12:10:05"
  },
  "result": {
    "read": {
      "count": 0,
      "min_us": 0,
      "max_us": 0,
      "avg_us": 0.00,
      "percentiles_us": {
        "p10": 0
      }
    }
  }
}
```

JSON field meanings:

- `query.api`: API function name (`get_read_latency_percentiles`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.read.count`: sample count
- `result.read.min_us`: minimum latency in microseconds
- `result.read.max_us`: maximum latency in microseconds
- `result.read.avg_us`: average latency in microseconds
- `result.read.percentiles_us.pXX`: percentile latency in microseconds

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_latency_percentiles("/dev/nvme0n1",
                                 "2026-04-26 10:05:05",
                                 "2026-04-26 12:10:05",
                                 json) == 0) {
    puts(json);
}
```

---

### 3.2 `get_write_latency_percentiles`

```c
int get_write_latency_percentiles(const char *device,
                                  const char *time_start,
                                  const char *time_end,
                                  char *json_buffer);
```

Function:

- Query write latency percentile statistics only.

Top-level JSON objects:

- `query`
- `result`

Example JSON shape:

```json
{
  "query": {
    "api": "get_write_latency_percentiles",
    "device": "/dev/nvme0n1",
    "time_start": "2026-04-26 10:05:05",
    "time_end": "2026-04-26 12:10:05"
  },
  "result": {
    "write": {
      "count": 0,
      "min_us": 0,
      "max_us": 0,
      "avg_us": 0.00,
      "percentiles_us": {
        "p10": 0
      }
    }
  }
}
```

JSON field meanings:

- `query.api`: API function name (`get_write_latency_percentiles`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.write.count`: sample count
- `result.write.min_us`: minimum latency in microseconds
- `result.write.max_us`: maximum latency in microseconds
- `result.write.avg_us`: average latency in microseconds
- `result.write.percentiles_us.pXX`: percentile latency in microseconds

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_latency_percentiles("/dev/nvme0n1",
                                  "2026-04-26 10:05:05",
                                  "2026-04-26 12:10:05",
                                  json) == 0) {
    puts(json);
}
```

---

### 3.3 `get_write_amplification`

```c
int get_write_amplification(const char *device,
                            const char *time_start,
                            const char *time_end,
                            char *json_buffer);
```

Function:

- Query a single write-amplification value from stat records within the time window.
- Calculation:
  `(sum(hot_write_4k) + sum(folding_write_4k)) / sum(hot_write_4k)`.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_write_amplification`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result`: computed WA value over all stat records in the window
  (`hot_write` and `folding_write` are both 4KiB-unit counters in stat records)

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_amplification("/dev/nvme0n1",
                            "2026-04-26 10:05:05",
                            "2026-04-26 12:10:05",
                            json) == 0) {
    puts(json);
}
```

---

### 3.4 `get_qd_distribution`

```c
int get_qd_distribution(const char *device,
                        const char *time_start,
                        const char *time_end,
                        char *json_buffer);
```

Function:

- Query queue-depth (QD) distribution from stat records.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_qd_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total QD samples counted
- `result.buckets[].label`: QD range label
- `result.buckets[].count`: sample count in this QD range
- `result.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_qd_distribution("/dev/nvme0n1",
                        "2026-04-26 10:05:05",
                        "2026-04-26 12:10:05",
                        json) == 0) {
    puts(json);
}
```

---

### 3.5 `get_read_size_distribution`

```c
int get_read_size_distribution(const char *device,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer);
```

Function:

- Query read I/O size distribution (range buckets).

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_read_size_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total read-size samples counted
- `result.buckets[].label`: range label (for example `(0,4K]`)
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_size_distribution("/dev/nvme0n1",
                               "2026-04-26 10:05:05",
                               "2026-04-26 12:10:05",
                               json) == 0) {
    puts(json);
}
```

---

### 3.6 `get_write_size_distribution`

```c
int get_write_size_distribution(const char *device,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer);
```

Function:

- Query write I/O size distribution (range buckets).

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_write_size_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total write-size samples counted
- `result.buckets[].label`: range label
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_size_distribution("/dev/nvme0n1",
                                "2026-04-26 10:05:05",
                                "2026-04-26 12:10:05",
                                json) == 0) {
    puts(json);
}
```

---

### 3.7 `get_read_throughput_distribution`

```c
int get_read_throughput_distribution(const char *device,
                                     const char *time_start,
                                     const char *time_end,
                                     char *json_buffer);
```

Function:

- Query read throughput distribution.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_read_throughput_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total read-throughput samples counted
- `result.buckets[].label`: throughput range label (GiB/s)
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_throughput_distribution("/dev/nvme0n1",
                                     "2026-04-26 10:05:05",
                                     "2026-04-26 12:10:05",
                                     json) == 0) {
    puts(json);
}
```

---

### 3.8 `get_write_throughput_distribution`

```c
int get_write_throughput_distribution(const char *device,
                                      const char *time_start,
                                      const char *time_end,
                                      char *json_buffer);
```

Function:

- Query write throughput distribution.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_write_throughput_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total write-throughput samples counted
- `result.buckets[].label`: throughput range label (GiB/s)
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_throughput_distribution("/dev/nvme0n1",
                                      "2026-04-26 10:05:05",
                                      "2026-04-26 12:10:05",
                                      json) == 0) {
    puts(json);
}
```

---

### 3.9 `get_read_count_distribution`

```c
int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer);
```

Function:

- Query request-based read-count distribution.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_read_count_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total read-count samples counted
- `result.buckets[].label`: read-count range label
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`
- `result.buckets[].MiB`: aggregated data size in MiB for this range

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_count_distribution("/dev/nvme0n1", 4096,
                                "2026-04-26 10:05:05",
                                "2026-04-26 12:10:05",
                                json) == 0) {
    puts(json);
}
```

---

### 3.10 `get_write_to_first_read_distribution`

```c
int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         char *json_buffer);
```

Function:

- Query write-to-first-read latency distribution.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_write_to_first_read_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total W2FR samples counted
- `result.buckets[].label`: latency range label
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_to_first_read_distribution("/dev/nvme0n1", 4096,
                                         "2026-04-26 10:05:05",
                                         "2026-04-26 12:10:05",
                                         json) == 0) {
    puts(json);
}
```

---

### 3.11 `get_lifecycle_distribution`

```c
int get_lifecycle_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer);
```

Function:

- Query lifecycle latency distribution.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_lifecycle_distribution`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.sampling`: total lifecycle samples counted
- `result.buckets[].label`: latency range label
- `result.buckets[].count`: sample count in this range
- `result.buckets[].ratio`: percentage of this range in `total`
- `result.buckets[].MiB`: aggregated data size in MiB for this range

Example JSON shape:

```json
{
  "query": {
    "api": "get_lifecycle_distribution",
    "device": "/dev/nvme0n1",
    "time_start": "2026-04-26 10:05:05",
    "time_end": "2026-04-26 12:10:05"
  },
  "result": {
    "sampling": 0,
    "buckets": [
      {"label":"[1s,5s)","count":0,"ratio":0.0,"MiB":0.0}
    ]
  }
}
```

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_lifecycle_distribution("/dev/nvme0n1", 4096,
                               "2026-04-26 10:05:05",
                               "2026-04-26 12:10:05",
                               json) == 0) {
    puts(json);
}
```

---

### 3.12 `get_nand_write_volume`

```c
int get_nand_write_volume(const char *device,
                          const char *time_start,
                          const char *time_end,
                          char *json_buffer);
```

Function:

- Query total NAND write volume from stat records in the input time window.
- The source field is `hot_write` from each stat record.
- `hot_write` is in 4KiB units, and output is converted to MiB.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_nand_write_volume`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.total`: formatted string value in `MiB` (for example
  `"67.89 MiB"`), converted from stat-record `hot_write` 4KiB units

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_nand_write_volume("/dev/nvme0n1",
                          "2026-04-26 10:05:05",
                          "2026-04-26 12:10:05",
                          json) == 0) {
    puts(json);
}
```

---

### 3.13 `get_gc_data_movement`

```c
int get_gc_data_movement(const char *device,
                         const char *time_start,
                         const char *time_end,
                         char *json_buffer);
```

Function:

- Query total GC data movement from stat records in the input time window.
- The source field is `folding_write` from each stat record.
- `folding_write` is in 4KiB units, and output is converted to MiB.

Top-level JSON objects:

- `query`
- `result`

JSON field meanings:

- `query.api`: API function name (`get_gc_data_movement`)
- `query.device`: input `device` value
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.total`: formatted string value in `MiB` (for example
  `"67.89 MiB"`), converted from stat-record `folding_write` 4KiB units

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_gc_data_movement("/dev/nvme0n1",
                         "2026-04-26 10:05:05",
                         "2026-04-26 12:10:05",
                         json) == 0) {
    puts(json);
}
```

---

## 4. Example programs

Full source for time-window and session-id usage is in the repository:

- `examples/insight_api_example.c`
- `examples/insight_api_session_example.c`

```bash
make examples
sudo ./insight_api_example /dev/nvme0n1 "2026-04-26 10:05:05" "2026-04-26 12:10:05" 4096
sudo ./insight_api_session_example /dev/nvme0n1 42 4096
```
