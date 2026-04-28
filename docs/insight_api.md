# Insight API Reference (`insight_api.h`)

This document describes all public APIs declared in `insight_api.h`.

## 1. Overview

The insight API library provides JSON-based query functions for NVMe insight data.

- Header: `insight_api.h`
- Static library: `libinsight_api.a`
- JSON output buffer size macro: `INSIGHT_JSON_BUFFER_BYTES` (65536 bytes)

All APIs share this signature pattern:

```c
int api_name(const char *device,
             uint64_t block_size,
             const char *time_start,
             const char *time_end,
             char *json_buffer);
```

## 2. Build and Link

From repository root:

```bash
make clean && make
```

Link your application with:

- `libinsight_api.a`
- `-lpthread`

## 3. Common Input Contract

### `device`

- NVMe device path (example: `/dev/nvme0n1`)
- Must not be `NULL`

### `block_size`

- Unit: bytes
- Same semantics as CLI `--block-size`
- Must be positive (`> 0`)

### `time_start`, `time_end`

- Format: `YYYY-MM-DD HH:MM:SS`
- Local time (`mktime` based)
- Must satisfy `time_start <= time_end`

### `json_buffer`

- Caller-provided output buffer
- Must not be `NULL`
- Recommended size: `INSIGHT_JSON_BUFFER_BYTES`

## 4. Return Value and Error Handling

All APIs return:

- `0`: success, JSON written into `json_buffer`
- `-1`: failure, `errno` set

Common `errno` values:

- `EINVAL`: invalid arguments or invalid time format/range
- `ERANGE`: scan range invalid/out of log range
- `ENODATA`: JSON payload not found in captured output
- `ENOSPC`: output JSON does not fit in `json_buffer`
- `ENOMEM`: allocation failure
- `EIO`: I/O or internal capture/restore failure

## 5. Thread Safety and Runtime Behavior

- APIs are serialized by an internal global mutex (thread-safe, but not parallelized).
- Each API call performs an independent scan and parse pass.
- Time-window filtering is marker-driven (based on marker unix timestamp in milliseconds).

## 6. Public APIs

## 6.1 `get_read_latency_percentiles`

```c
int get_read_latency_percentiles(...);
```

CLI equivalence:

```bash
./sfx_ctx_insight_analyze --format-json --latency --block-size=<block_size> \
  -S <time_start> -E <time_end> <device> 0 11T
```

Top-level JSON key:

- `latency_percentiles`

Schema:

```json
{
  "latency_percentiles": {
    "read|write|trim": {
      "count": 0,
      "min_us": 0,
      "max_us": 0,
      "avg_us": 0.0,
      "percentiles_us": {
        "p10": 0,
        "p20": 0,
        "p30": 0,
        "p40": 0,
        "p50": 0,
        "p60": 0,
        "p70": 0,
        "p80": 0,
        "p90": 0,
        "p99": 0,
        "p99_9": 0,
        "p99_99": 0
      }
    }
  }
}
```

## 6.2 `get_write_amplification`

```c
int get_write_amplification(...);
```

Top-level key: `wa_dist`

```json
{
  "wa_dist": {
    "total": 0,
    "buckets": [
      {"label":"1.0","count":0,"ratio":0.0}
    ]
  }
}
```

Bucket labels:

`1.0`, `1.1-1.5`, `1.6-2.0`, `2.1-2.5`, `2.6-3.0`,
`3.1-3.5`, `3.6-4.0`, `4.1-4.5`, `4.6-5.0`, `>5.0`

## 6.3 `get_qd_distribution`

```c
int get_qd_distribution(...);
```

Top-level key: `qd_dist`

Bucket labels:

`0`, `1`, `2`, `3-4`, `5-8`, `9-16`, `17-32`, `33-64`, `65-128`, `>=129`

## 6.4 `get_read_size_distribution`

```c
int get_read_size_distribution(...);
```

Top-level key: `read_size_dist`

Range bucket labels:

`0`, `(0,4K]`, `(4K,8K]`, `(8K,16K]`, `(16K,32K]`,
`(32K,64K]`, `(64K,128K]`, `(128K,256K]`, `(256K,512K]`, `>512K`

## 6.5 `get_write_size_distribution`

```c
int get_write_size_distribution(...);
```

Top-level key: `write_size_dist`

Range bucket labels are the same as `read_size_dist`.

## 6.6 `get_read_throughput_distribution`

```c
int get_read_throughput_distribution(...);
```

Top-level key: `read_throughput_dist`

Bucket granularity:

- strict `1GiB/s` bins through `16GiB/s`
- labels: `0-1`, `1-2`, ..., `15-16`, `>=16GiB/s`

## 6.7 `get_write_throughput_distribution`

```c
int get_write_throughput_distribution(...);
```

Top-level key: `write_throughput_dist`

Bucket semantics are the same as `read_throughput_dist`.

## 6.8 `get_read_count_distribution`

```c
int get_read_count_distribution(...);
```

Top-level key: `read_count_distribution`

Schema:

```json
{
  "read_count_distribution": {
    "total": 0,
    "buckets": [
      {"label":"1-25","count":0,"ratio":0.0,"bytes_mib":0.0}
    ]
  }
}
```

Read-count bucket labels:

`0`, `1-25`, `26-50`, `51-75`, `76-100`,
`101-125`, `126-150`, `151-175`, `176-200`, `201-255`

## 6.9 `get_write_to_first_read_distribution`

```c
int get_write_to_first_read_distribution(...);
```

Top-level key: `write_to_first_read_distribution`

Latency bucket labels:

`0ms`, `(0ms,1s)`, `[1s,5s)`, `[5s,10s)`, `[10s,30s)`,
`[30s,1m)`, `[1m,5m)`, `[5m,30m)`, `[30m,1h)`, `[1h,4h)`, `>=4h`

## 6.10 `get_lifecycle_distribution`

```c
int get_lifecycle_distribution(...);
```

Top-level key: `lifecycle_distribution`

Same latency bucket labels as `get_write_to_first_read_distribution`.

Each bucket includes:

- `count`
- `ratio`
- `bytes_mib`

## 7. Example Program

See:

- `examples/insight_api_example.c`

Run:

```bash
sudo ./insight_api_example /dev/nvme0n1 "2026-04-26 10:05:05" "2026-04-26 12:10:05"
```

The example invokes all public APIs and prints each returned JSON payload.
