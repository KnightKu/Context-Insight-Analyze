# Insight API Reference (`insight_api.h`)

This document is the authoritative API reference for all public interfaces in
`insight_api.h`.

## 1. Build and Link

Build from repository root:

```bash
make clean && make
```

Include and link:

- Header: `insight_api.h`
- Static library: `libinsight_api.a`
- Shared library: `libinsight_api.so`
- Linker flag: `-lpthread`

### 1.1 Build shared library

```bash
make libinsight_api.so
```

### 1.2 Minimal dynamic-link demo (full flow: compile -> run)

Minimal demo source:

- `examples/insight_api_dynamic_demo.c`

Build demo against shared library:

```bash
make insight_api_dynamic_demo
```

Run demo (dynamic linker needs current directory):

```bash
export LD_LIBRARY_PATH="$(pwd):${LD_LIBRARY_PATH}"
sudo ./insight_api_dynamic_demo /dev/nvme0n1 "2026-04-26 10:05:05" "2026-04-26 12:10:05" 4096
```

One-line compile command (without Makefile target):

```bash
gcc -Wall -Wextra -pedantic -std=c11 -I. \
  -o insight_api_dynamic_demo examples/insight_api_dynamic_demo.c \
  -L. -linsight_api -lpthread
```

Note:

- If runtime reports `libinsight_api.so: cannot open shared object file`,
  set `LD_LIBRARY_PATH` as shown above or install the shared library to a
  standard library path.

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
  - `make clean && make CFLAGS="-Wall -Wextra -pedantic -std=c11 -I. -DINSIGHT_API_PERF_DEBUG=1 -DINSIGHT_PERF_DEBUG=1"`
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

## 4. Complete Demo Program (embedded source)

The full demo source from `examples/insight_api_example.c` is embedded below, so you can
copy, compile, and run directly from this document.

```c
#include "insight_api.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u64_str(const char *s, uint64_t *out) {
    if (s == NULL || out == NULL || *s == '\0') {
        return -1;
    }
    char *endptr = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &endptr, 10);
    if (errno != 0 || endptr == s || *endptr != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr,
                "usage: %s <device> <time_start \"YYYY-MM-DD HH:MM:SS\"> "
                "<time_end \"YYYY-MM-DD HH:MM:SS\"> [block_size_bytes]\n",
                argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const char *time_start = argv[2];
    const char *time_end = argv[3];
    uint64_t block_size = 4096ULL;
    if (argc == 5) {
        if (parse_u64_str(argv[4], &block_size) != 0 || block_size == 0ULL) {
            fprintf(stderr, "invalid block_size_bytes: %s\n", argv[4]);
            return 1;
        }
    }

    char json_buffer[INSIGHT_JSON_BUFFER_BYTES];

    if (get_read_latency_percentiles(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_latency_percentiles failed: %s\n", strerror(errno));
        return 2;
    }
    printf("=== read_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_latency_percentiles(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_latency_percentiles failed: %s\n", strerror(errno));
        return 3;
    }
    printf("=== write_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_amplification(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_amplification failed: %s\n", strerror(errno));
        return 4;
    }
    printf("=== write_amplification ===\n%s\n\n", json_buffer);

    if (get_qd_distribution(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_qd_distribution failed: %s\n", strerror(errno));
        return 5;
    }
    printf("=== qd_distribution ===\n%s\n\n", json_buffer);

    if (get_read_size_distribution(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_size_distribution failed: %s\n", strerror(errno));
        return 6;
    }
    printf("=== read_size_distribution ===\n%s\n\n", json_buffer);

    if (get_write_size_distribution(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_size_distribution failed: %s\n", strerror(errno));
        return 7;
    }
    printf("=== write_size_distribution ===\n%s\n\n", json_buffer);

    if (get_read_throughput_distribution(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_throughput_distribution failed: %s\n", strerror(errno));
        return 8;
    }
    printf("=== read_throughput_distribution ===\n%s\n\n", json_buffer);

    if (get_write_throughput_distribution(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_throughput_distribution failed: %s\n", strerror(errno));
        return 9;
    }
    printf("=== write_throughput_distribution ===\n%s\n", json_buffer);

    if (get_read_count_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_count_distribution failed: %s\n", strerror(errno));
        return 10;
    }
    printf("=== read_count_distribution ===\n%s\n\n", json_buffer);

    if (get_write_to_first_read_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_to_first_read_distribution failed: %s\n", strerror(errno));
        return 11;
    }
    printf("=== write_to_first_read_distribution ===\n%s\n\n", json_buffer);

    if (get_lifecycle_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_lifecycle_distribution failed: %s\n", strerror(errno));
        return 12;
    }
    printf("=== lifecycle_distribution ===\n%s\n\n", json_buffer);

    if (get_nand_write_volume(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_nand_write_volume failed: %s\n", strerror(errno));
        return 13;
    }
    printf("=== nand_write_volume ===\n%s\n\n", json_buffer);

    if (get_gc_data_movement(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_gc_data_movement failed: %s\n", strerror(errno));
        return 14;
    }
    printf("=== gc_data_movement ===\n%s\n", json_buffer);

    return 0;
}
```

Build and run this complete demo:

```bash
make insight_api_example
sudo ./insight_api_example /dev/nvme0n1 "2026-04-26 10:05:05" "2026-04-26 12:10:05"
```
