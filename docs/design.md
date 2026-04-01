# NVMe Reader Post Action Design Document

## 1. Background and Goals

This project reads data via Linux NVMe passthrough and runs a `post action` callback after each read chunk.  
This document focuses on the `post action` subsystem design and its goals:

- Parse multiple record formats according to the protocol (8B / 16B)
- Reconstruct fields precisely in little-endian order
- Fail fast on malformed/truncated data with actionable errors
- Provide optional debug observability without affecting the default fast path

## 2. Design Constraints

- The read path is performance-sensitive; default parsing must stay lightweight.
- `post action` is executed synchronously inside the `nvme_read` loop; any error stops the read flow.
- Input is a raw byte stream; record boundaries must be determined by both length and opcode.
- The protocol includes non-standard field widths (3B / 5B / 7B), requiring a unified little-endian decoder.

## 3. Protocol and Record Definitions

The current parser supports 5 opcodes and two record lengths:

- 16-byte records: `0x01` (Read), `0x02` (Write), `0x03` (Trim)
- 8-byte records: `0x0F` (Stat), `0xFF` (Marker)

### 3.1 Read / Write (`0x01` / `0x02`) - 16B

| Field | Size | Offset | Description |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0x01` for read, `0x02` for write |
| Start LBA | 5B | 1 | Little-endian, decoded into `uint64_t` with high-bit masking |
| Length | 2B | 6 | Requested length |
| Reserved | 2B | 8 | Must be `0x0000` |
| Latency | 3B | 10 | Command latency |
| Time | 3B | 13 | Relative timestamp |

### 3.2 Trim (`0x03`) - 16B

| Field | Size | Offset | Description |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0x03` |
| Start LBA | 5B | 1 | Range start LBA |
| Total ranges | 1B | 6 | Total ranges in this Trim command (protocol says up to 256) |
| Range index | 1B | 7 | Current range index |
| Reserved | 1B | 8 | Must be `0x00` |
| Length | 4B | 9 | Current range length |
| Time | 3B | 13 | Timestamp |

> If `Total ranges > 1`, consecutive Trim records represent one logical Trim operation.

### 3.3 Stat (`0x0F`) - 8B

| Field | Size | Offset | Description |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0x0F` |
| QD | 2B | 1 | Queue depth |
| WA | 1B | 3 | Write amplification |
| Reserved | 1B | 4 | Must be `0x00` |
| Time | 3B | 5 | Relative timestamp |

### 3.4 Marker (`0xFF`) - 8B

| Field | Size | Offset | Description |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0xFF` |
| Absolute time | 7B | 1 | High-precision absolute timestamp (microseconds) |

### 3.5 Timestamp Semantics

- `Marker.abs_time` is an absolute timestamp in microseconds (`us`).
- For `Read/Write/Trim/Stat`, the 3-byte `time` field is interpreted as a relative delta (`time_rel`)
  against the most recent preceding marker.
- Effective timestamp:
  - `abs_time_us = last_marker_abs_time_us + time_rel`
- If a non-marker record appears before any marker, the record is treated as invalid by parser rules.

## 4. Core Algorithm

### 4.1 Fast Endianness Decoder

The parser now uses `load_le64_u(const unsigned char *p)` as the hot-path primitive:

- Loads one unaligned 8-byte word via `memcpy`
- Uses host-endian branch (no swap on little-endian hosts)
- Decodes 8-byte records with one load and 16-byte records with two loads
- Extracts fields with bit masks and shifts

### 4.2 Record Dispatch and Boundary Safety

`default_post_action` uses this flow:

1. Validate `data != NULL` and alignment constraints
2. Start scanning with `cursor = 0`
3. Read `op = bytes[cursor]`
4. Determine record size (8 or 16) from opcode
5. Check termination marker:
   - If at least 16 bytes remain
   - And the first byte of both 8-byte halves is `0x00`
   - Stop parsing and return success
6. Dispatch to one parser:
   - `parse_rw_record`
   - `parse_trim_group`
   - `parse_stat_record`
   - `parse_marker_record`
7. Move `cursor += record_size` and continue

### 4.3 Read/Process Pipeline (Producer-Consumer)

The read path now uses a two-stage pipeline:

- **Reader thread (producer)**:
  - Issues NVMe read commands
  - Writes chunk payload into ring slots
  - Pushes ready slots to the consumer queue
- **Post-action thread (consumer)**:
  - Pops ready slots
  - Runs `nvme_post_action_process(...)`
  - Returns slots back to free queue

Implementation notes:

- Ring depth: `NVME_READ_PIPELINE_SLOTS` (currently 4)
- Per-slot buffer size: `read_chunk_bytes`
- Synchronization: `pthread_mutex_t` + condition variables (`cv_free`, `cv_ready`)
- Error policy:
  - Any producer/consumer error sets stop flag and broadcasts wakeups
  - Pipeline exits and returns failure to `nvme_read`

### 4.4 Validation Rules

- Unknown opcode -> counted as invalid and skipped by one aligned record unit
- Non-zero reserved field -> counted as invalid and skipped
- Missing marker reference for relative timestamp -> counted as invalid and skipped
- Non-8-byte-aligned tail bytes -> counted as invalid tail fragment
- Truncated record/group -> counted as invalid and skipped conservatively

## 5. Error Handling Strategy

- Parser-level invalid records do not abort the whole chunk processing path.
- Invalid records are counted and skipped so parsing can continue.
- `nvme_read` aborts only on structural/runtime failures outside tolerated invalid-record scope
  (for example, I/O failure, thread/setup failure).
- Logs still include `offset`, `record index`, `opcode`, and field context for troubleshooting.
- Pipeline thread failures preserve `errno` and are propagated to the caller

## 6. Debug and Observability

Compile-time macro `NVME_POST_ACTION_DEBUG` controls debug logs:

- Default: `0` (off)
- Set to `1` to print parsed fields for each record type
- When `data_len < 8`, the "no complete 8-byte unit" message is printed only in debug mode
- When termination marker is hit, a debug line is printed (in debug mode)
- Invalid record count is reported in final debug read stats (`invalid_records=...`)

Compile-time macro `NVME_POST_ACTION_SKIP_STAT` controls whether Stat (`0x0F`) records
are parsed:

- Default: `0` (off), Stat records are parsed and validated as before.
- Set to `1` to skip Stat processing in post action:
  - Stat records are consumed as 8-byte units
  - No Stat field validation is performed
  - No invalid-record count is added for skipped Stat records

Runtime latency output switch:

- CLI option `-l` / `--latency` enables fio-like latency summary printing for
  `read`, `write`, and `trim` records.
- Metrics are collected from parser-side record latency fields:
  - `count`, `min`, `max`, `avg`
  - `p50`, `p90`, `p99` percentiles
- Output unit is microseconds (`us`), and aggregation is enabled only when latency mode is on.

## 7. LBA Post-Action Statistics (5-Byte Bucket Model)

The post-action path now includes an in-memory LBA statistics engine.

### 7.1 Bucket Granularity and Memory Layout

- Bucket granularity: **4 KiB logical address window**
- Per-bucket storage: **5 bytes**
  - `read_count` (1 byte)
  - `write_to_first_read_latency` (2 bytes)
  - `life_cycle_latency` (2 bytes)

By default, total logical address space is treated as 4 TiB:

- `4 TiB / 4 KiB = 1,073,741,824` buckets
- `1,073,741,824 * 5 B = 5,368,709,120 B` (~5 GiB virtual memory)

The storage is initialized by `mmap` (with `MAP_NORESERVE` when available) to keep startup cost low.

### 7.2 Unit Semantics

- In records, both `start_lba` and `length` are interpreted in units of `sector_size`.
- Default `sector_size` is 512 bytes; runtime-detected sector size is used when available.
- Bucket mapping:
  - `start_bytes = start_lba * sector_size`
  - `end_bytes = (start_lba + length) * sector_size`
  - all covered 4 KiB buckets are updated.

### 7.3 Counters and Latency Encoding

1. **Read Count (1B)**
   - Counts reads that occur after at least one write in the bucket.
   - Uses saturating counter semantics (`max = 255`).

2. **Write-to-First-Read Latency (2B code)**
   - Measured from latest write to first subsequent read.
   - If no prior write exists, latency remains `0`.
   - Encoded with non-linear lookup-table mapping in milliseconds.

3. **LBA Life Cycle (2B code)**
   - Measured from one write to the next overwrite write.
   - Encoded with the same non-linear lookup-table mapping.

### 7.4 Non-Linear LUT Encoding

- Input unit: milliseconds (converted from microsecond timestamps).
- Output: `uint16_t` code (`0..65535`).
- Mapping is monotonic and super-linear (step size grows by code range), covering
  millisecond scale to multi-hour scale.
- Values above the table upper bound are saturated to `65535`.

### 7.5 Statistics Export API (CSV by Bucket Range)

A public export API is available:

```c
int nvme_post_action_export_stats_csv(const char *csv_path,
                                      uint64_t start_bucket,
                                      uint64_t bucket_count);
```

Behavior:

- Exports only the requested bucket range to CSV.
- Header format:
  - `bucket_index,read_count,write_to_first_read_latency_code,life_cycle_latency_code`
- One row per bucket in `[start_bucket, start_bucket + bucket_count)`.
- If range exceeds available buckets, export is clamped to the valid tail.
- Returns `0` on success, `-1` on failure (`errno` set).

### 7.6 Advanced LBA Life-Cycle Group Statistics

Advanced overwrite life-cycle grouping is supported for covered write ranges:

- Scope: grouped overwrite statistics based on per-4KiB bucket life-cycle codes.
- Group size bins: `4K, 8K, 16K, ...` up to current read `mdts` size.
- Alignment rule:
  - group analysis is 4KiB aligned.
  - non-4KiB-aligned write length is rounded down to covered full 4KiB groups.
  - example: `12K` covered write contributes to `8K` group analysis.
- Counting rule:
  - if all 4KiB buckets inside one group share the same life-cycle encoded value,
    that `(group_size, life_cycle_code)` counter is incremented by 1.
- Life-cycle encoding reuses the same non-linear LUT (`ms -> code`) used by bucket life-cycle.

## 8. Extensibility

To add a new opcode:

1. Add opcode constant
2. Implement parser function
3. Extend dispatch switch with record size and parser mapping

For future variable-length records, parse a minimal header first, then compute dynamic record size.

## 9. Trim Group Aggregation Model

`Trim (0x03)` records are aggregated into one logical object:

- Structure: `nvme_post_action_trim_t`
  - `meta` (op, offset, record index)
  - `total_ranges`
  - `range_count`
  - `ranges[256]`
- Each entry in `ranges[]` stores:
  - `start_lba`
  - `range_index`
  - `length`
  - `time_rel`

Special handling:

- Field width for `total_ranges` is 1 byte, but protocol max is 256.
- Raw `total_ranges == 0` is interpreted as `256`.

Consistency checks during aggregation:

- Every grouped record must remain opcode `0x03`
- Every grouped record must have consistent `total_ranges`
- Every grouped record must satisfy reserved-field constraints

## 10. Known Limits and Recommendations

- Current implementation validates structure and reserved fields only; it does not enforce cross-record semantics (for example, Trim range sequence consistency).
- Recommended next steps:
  - Add unit tests per opcode (valid/invalid)
  - Add truncated/misaligned record tests
  - Add reserved-field non-zero tests
  - Add mixed-record stream tests
