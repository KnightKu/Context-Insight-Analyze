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

Output envelope note:

- `get_lifecycle_distribution`, `get_nand_write_volume`, and
  `get_gc_data_movement` return an extended envelope:
  - `query`: function name and input parameters
  - `result`: API result payload
- Other APIs keep their original top-level result objects.

---

## 3. API Details

### 3.1 `get_read_latency_percentiles`

```c
int get_read_latency_percentiles(const char *device,
                                 uint64_t block_size,
                                 const char *time_start,
                                 const char *time_end,
                                 char *json_buffer);
```

Function:

- Query read/write/trim latency summary and percentile statistics.

Top-level JSON object:

- `latency_percentiles`

JSON field meanings:

- `latency_percentiles.read|write|trim.count`: sample count
- `latency_percentiles.read|write|trim.min_us`: minimum latency in microseconds
- `latency_percentiles.read|write|trim.max_us`: maximum latency in microseconds
- `latency_percentiles.read|write|trim.avg_us`: average latency in microseconds
- `latency_percentiles.read|write|trim.percentiles_us.pXX`: percentile latency in microseconds

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_latency_percentiles("/dev/nvme0n1", 4096,
                                 "2026-04-26 10:05:05",
                                 "2026-04-26 12:10:05",
                                 json) == 0) {
    puts(json);
}
```

---

### 3.2 `get_write_amplification`

```c
int get_write_amplification(const char *device,
                            uint64_t block_size,
                            const char *time_start,
                            const char *time_end,
                            char *json_buffer);
```

Function:

- Query write-amplification distribution from stat records.

Top-level JSON object:

- `wa_dist`

JSON field meanings:

- `wa_dist.total`: total WA samples counted
- `wa_dist.buckets[].label`: WA range label
- `wa_dist.buckets[].count`: sample count in this WA range
- `wa_dist.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_amplification("/dev/nvme0n1", 4096,
                            "2026-04-26 10:05:05",
                            "2026-04-26 12:10:05",
                            json) == 0) {
    puts(json);
}
```

---

### 3.3 `get_qd_distribution`

```c
int get_qd_distribution(const char *device,
                        uint64_t block_size,
                        const char *time_start,
                        const char *time_end,
                        char *json_buffer);
```

Function:

- Query queue-depth (QD) distribution from stat records.

Top-level JSON object:

- `qd_dist`

JSON field meanings:

- `qd_dist.total`: total QD samples counted
- `qd_dist.buckets[].label`: QD range label
- `qd_dist.buckets[].count`: sample count in this QD range
- `qd_dist.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_qd_distribution("/dev/nvme0n1", 4096,
                        "2026-04-26 10:05:05",
                        "2026-04-26 12:10:05",
                        json) == 0) {
    puts(json);
}
```

---

### 3.4 `get_read_size_distribution`

```c
int get_read_size_distribution(const char *device,
                               uint64_t block_size,
                               const char *time_start,
                               const char *time_end,
                               char *json_buffer);
```

Function:

- Query read I/O size distribution (range buckets).

Top-level JSON object:

- `read_size_dist`

JSON field meanings:

- `read_size_dist.total`: total read-size samples counted
- `read_size_dist.buckets[].label`: range label (for example `(0,4K]`)
- `read_size_dist.buckets[].count`: sample count in this range
- `read_size_dist.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_size_distribution("/dev/nvme0n1", 4096,
                               "2026-04-26 10:05:05",
                               "2026-04-26 12:10:05",
                               json) == 0) {
    puts(json);
}
```

---

### 3.5 `get_write_size_distribution`

```c
int get_write_size_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer);
```

Function:

- Query write I/O size distribution (range buckets).

Top-level JSON object:

- `write_size_dist`

JSON field meanings:

- `write_size_dist.total`: total write-size samples counted
- `write_size_dist.buckets[].label`: range label
- `write_size_dist.buckets[].count`: sample count in this range
- `write_size_dist.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_size_distribution("/dev/nvme0n1", 4096,
                                "2026-04-26 10:05:05",
                                "2026-04-26 12:10:05",
                                json) == 0) {
    puts(json);
}
```

---

### 3.6 `get_read_throughput_distribution`

```c
int get_read_throughput_distribution(const char *device,
                                     uint64_t block_size,
                                     const char *time_start,
                                     const char *time_end,
                                     char *json_buffer);
```

Function:

- Query read throughput distribution.

Top-level JSON object:

- `read_throughput_dist`

JSON field meanings:

- `read_throughput_dist.total`: total read-throughput samples counted
- `read_throughput_dist.buckets[].label`: throughput range label (GiB/s)
- `read_throughput_dist.buckets[].count`: sample count in this range
- `read_throughput_dist.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_read_throughput_distribution("/dev/nvme0n1", 4096,
                                     "2026-04-26 10:05:05",
                                     "2026-04-26 12:10:05",
                                     json) == 0) {
    puts(json);
}
```

---

### 3.7 `get_write_throughput_distribution`

```c
int get_write_throughput_distribution(const char *device,
                                      uint64_t block_size,
                                      const char *time_start,
                                      const char *time_end,
                                      char *json_buffer);
```

Function:

- Query write throughput distribution.

Top-level JSON object:

- `write_throughput_dist`

JSON field meanings:

- `write_throughput_dist.total`: total write-throughput samples counted
- `write_throughput_dist.buckets[].label`: throughput range label (GiB/s)
- `write_throughput_dist.buckets[].count`: sample count in this range
- `write_throughput_dist.buckets[].ratio`: percentage of this range in `total`

Demo:

```c
char json[INSIGHT_JSON_BUFFER_BYTES];
if (get_write_throughput_distribution("/dev/nvme0n1", 4096,
                                      "2026-04-26 10:05:05",
                                      "2026-04-26 12:10:05",
                                      json) == 0) {
    puts(json);
}
```

---

### 3.8 `get_read_count_distribution`

```c
int get_read_count_distribution(const char *device,
                                uint64_t block_size,
                                const char *time_start,
                                const char *time_end,
                                char *json_buffer);
```

Function:

- Query request-based read-count distribution.

Top-level JSON object:

- `read_count_distribution`

JSON field meanings:

- `read_count_distribution.total`: total read-count samples counted
- `read_count_distribution.buckets[].label`: read-count range label
- `read_count_distribution.buckets[].count`: sample count in this range
- `read_count_distribution.buckets[].ratio`: percentage of this range in `total`
- `read_count_distribution.buckets[].bytes_mib`: aggregated data size in MiB for this range

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

### 3.9 `get_write_to_first_read_distribution`

```c
int get_write_to_first_read_distribution(const char *device,
                                         uint64_t block_size,
                                         const char *time_start,
                                         const char *time_end,
                                         char *json_buffer);
```

Function:

- Query write-to-first-read latency distribution.

Top-level JSON object:

- `write_to_first_read_distribution`

JSON field meanings:

- `write_to_first_read_distribution.total`: total W2FR samples counted
- `write_to_first_read_distribution.buckets[].label`: latency range label
- `write_to_first_read_distribution.buckets[].count`: sample count in this range
- `write_to_first_read_distribution.buckets[].ratio`: percentage of this range in `total`

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

### 3.10 `get_lifecycle_distribution`

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
- `query.block_size`: input `block_size` value (bytes)
- `query.time_start`: input `time_start` value
- `query.time_end`: input `time_end` value
- `result.lifecycle_distribution.total`: total lifecycle samples counted
- `result.lifecycle_distribution.buckets[].label`: latency range label
- `result.lifecycle_distribution.buckets[].count`: sample count in this range
- `result.lifecycle_distribution.buckets[].ratio`: percentage of this range in `total`
- `result.lifecycle_distribution.buckets[].bytes_mib`: aggregated data size in MiB for this range

Example JSON shape:

```json
{
  "query": {
    "api": "get_lifecycle_distribution",
    "device": "/dev/nvme0n1",
    "block_size": 4096,
    "time_start": "2026-04-26 10:05:05",
    "time_end": "2026-04-26 12:10:05"
  },
  "result": {
    "lifecycle_distribution": {
      "total": 0,
      "buckets": [
        {"label":"[1s,5s)","count":0,"ratio":0.0,"bytes_mib":0.0}
      ]
    }
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

### 3.11 `get_nand_write_volume`

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
- `result.nand_write_volume.unit`: always `MiB`
- `result.nand_write_volume.total_mib`: sum of all stat-record `hot_write` values
  within the time window, converted from 4KiB units to MiB

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

### 3.12 `get_gc_data_movement`

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
- `result.gc_data_movement.unit`: always `MiB`
- `result.gc_data_movement.total_mib`: sum of all stat-record `folding_write`
  values within the time window, converted from 4KiB units to MiB

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

    if (get_read_latency_percentiles(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_latency_percentiles failed: %s\n", strerror(errno));
        return 2;
    }
    printf("=== read_latency_percentiles ===\n%s\n\n", json_buffer);

    if (get_write_amplification(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_amplification failed: %s\n", strerror(errno));
        return 3;
    }
    printf("=== write_amplification ===\n%s\n\n", json_buffer);

    if (get_qd_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_qd_distribution failed: %s\n", strerror(errno));
        return 4;
    }
    printf("=== qd_distribution ===\n%s\n\n", json_buffer);

    if (get_read_size_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_size_distribution failed: %s\n", strerror(errno));
        return 5;
    }
    printf("=== read_size_distribution ===\n%s\n\n", json_buffer);

    if (get_write_size_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_size_distribution failed: %s\n", strerror(errno));
        return 6;
    }
    printf("=== write_size_distribution ===\n%s\n\n", json_buffer);

    if (get_read_throughput_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_throughput_distribution failed: %s\n", strerror(errno));
        return 7;
    }
    printf("=== read_throughput_distribution ===\n%s\n\n", json_buffer);

    if (get_write_throughput_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_throughput_distribution failed: %s\n", strerror(errno));
        return 8;
    }
    printf("=== write_throughput_distribution ===\n%s\n", json_buffer);

    if (get_read_count_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_read_count_distribution failed: %s\n", strerror(errno));
        return 9;
    }
    printf("=== read_count_distribution ===\n%s\n\n", json_buffer);

    if (get_write_to_first_read_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_write_to_first_read_distribution failed: %s\n", strerror(errno));
        return 10;
    }
    printf("=== write_to_first_read_distribution ===\n%s\n\n", json_buffer);

    if (get_lifecycle_distribution(device, block_size, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_lifecycle_distribution failed: %s\n", strerror(errno));
        return 11;
    }
    printf("=== lifecycle_distribution ===\n%s\n\n", json_buffer);

    if (get_nand_write_volume(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_nand_write_volume failed: %s\n", strerror(errno));
        return 12;
    }
    printf("=== nand_write_volume ===\n%s\n\n", json_buffer);

    if (get_gc_data_movement(device, time_start, time_end, json_buffer) != 0) {
        fprintf(stderr, "get_gc_data_movement failed: %s\n", strerror(errno));
        return 13;
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
