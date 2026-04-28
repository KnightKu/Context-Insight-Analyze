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
- Linker flag: `-lpthread`

## 2. Common Contract

All APIs use the same parameter pattern:

- `device`: NVMe device path (for example `/dev/nvme0n1`), must not be `NULL`
- `block_size`: bytes, must be positive, semantics equivalent to CLI `--block-size`
- `time_start` / `time_end`: format `YYYY-MM-DD HH:MM:SS`, and `time_start <= time_end`
- `json_buffer`: caller-provided output buffer, must not be `NULL`
  - recommended size: `INSIGHT_JSON_BUFFER_BYTES` (65536)

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

Top-level JSON object:

- `lifecycle_distribution`

JSON field meanings:

- `lifecycle_distribution.total`: total lifecycle samples counted
- `lifecycle_distribution.buckets[].label`: latency range label
- `lifecycle_distribution.buckets[].count`: sample count in this range
- `lifecycle_distribution.buckets[].ratio`: percentage of this range in `total`
- `lifecycle_distribution.buckets[].bytes_mib`: aggregated data size in MiB for this range

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

## 4. Complete Demo Program

A complete runnable sample that calls all APIs is provided in:

- `examples/insight_api_example.c`

Build and run:

```bash
make insight_api_example
sudo ./insight_api_example /dev/nvme0n1 "2026-04-26 10:05:05" "2026-04-26 12:10:05"
```
