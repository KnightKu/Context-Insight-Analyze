# NVMe Reader Architecture Document

## 1. High-Level Architecture

The project follows a layered structure:

1. **CLI Layer (`main.c`)**
   - Parses user arguments (`device_name`, `slba`, `data_len`)
   - Supports `K/M/G/T` units
   - Invokes the core API `nvme_read(...)`

2. **Core Read Pipeline (`nvme_read.c`)**
   - Opens the device and validates parameters
   - Detects device capabilities (sector size, MDTS)
   - Executes chunk-based NVMe passthrough reads
   - Invokes post action on each read chunk
   - Computes and prints bandwidth statistics

3. **Extension Layer (Post Action)**
   - Exposed via callback type `nvme_read_post_action_t`
   - Default implementation parses and validates protocol records
   - Supports grouped Trim object aggregation (`ranges[]`) and stream termination marker
   - Supports user-provided custom callbacks

---

## 2. Modules and Responsibilities

### 2.1 `main.c`

- `parse_u64_with_unit(...)`
  - Parses values such as `10K`, `64M`, `1G`, `1T`
  - Handles invalid suffixes and overflow

- `main(...)`
  - Validates command-line arguments
  - Parses `slba` and `data_len`
  - Calls `nvme_read(...)`

### 2.2 `nvme_read.h`

- Constants:
  - `NVME_READ_CHUNK_BYTES`
  - `NVME_SPLIT_BYTES`
  - `NVME_DEFAULT_DATA_LEN`
  - `NVME_LBA_SIZE_BYTES`

- Types:
  - `nvme_read_post_action_t`

- Public APIs:
  - `nvme_read_set_post_action(...)`
  - `nvme_read(...)`

### 2.3 `nvme_read.c`

- Capability probing:
  - `get_sector_size_or_default(...)`
  - `get_mdts_chunk_bytes_or_default(...)`
- Producer-consumer read pipeline orchestration
- Public API glue for read entrypoint and post-action callbacks

### 2.4 `post_action.c`

- Owns default post-action parser and callback dispatch
- Implements record parsing/validation (`rw`, `trim`, `stat`, `marker`)
- Handles termination marker and invalid-record counting
- Supports stat-skip compile-time macro (`NVME_POST_ACTION_SKIP_STAT`)

### 2.5 `post_action_stats.c`

- Owns per-bucket statistics state and update logic
- Manages 5-byte bucket model / non-linear latency encoding
- Provides stats summary and bucket access interfaces

### 2.6 `post_action_latency.c`

- Owns read/write/trim latency statistics aggregation
- Prints fio-style latency summary with count/min/max/avg and extended percentiles
- Runtime controlled by CLI option `-l` / `--latency`

---

## 3. Execution Sequence

Main read sequence:

1. `main` calls `nvme_read`
2. Open NVMe device node
3. Detect logical sector size (`BLKSSZGET`)
4. Query MDTS and determine chunk size
5. Submit `NVME_IOCTL_IO_CMD` in a loop
6. Run `nvme_post_action_process(...)` after each chunk
7. Print statistics and release resources

---

## 4. Data Flow

1. Input argument flow
   - CLI string arguments -> numeric values (`uint64_t`)

2. Device data flow
   - Device -> `chunk_buf` (memory)
   - `chunk_buf` -> post action (parse/validate)
   - Trim multi-range records -> one grouped in-memory Trim object with `ranges[]`
   - Optional LBA statistics path:
     - Bucket granularity: 4KiB (`4096B`)
     - Per-bucket footprint: 5 bytes (`read_count`, `write_to_first_read_latency`, `life_cycle_latency`)
     - Default logical coverage: 4TiB with default sector size `512B`
     - Total footprint: 5GiB (allocated via anonymous mmap)
     - `read_count` uses saturating counter (`max=255`)
     - Latency fields use non-linear encoding via lookup table

3. Diagnostic output flow
   - Errors printed to `stderr`
   - Debug logs controlled by macro

---

## 5. Error-Handling Strategy

### 5.1 General Policy

- Set `errno` on failure (e.g., `EINVAL`, `EIO`)
- Print contextual diagnostics (offset, record index, opcode)
- Release all allocated resources promptly (buffer, file descriptors)

### 5.2 Input and Record Validation

- `data_len` alignment requirements
- Record completeness checks in post action
- Reserved-field zero checks
- Opcode whitelist checks
- Trim-group consistency checks (`total_ranges` continuity across ranges)
- Termination marker check (`0x00` head byte in both 8-byte halves of a 16-byte window)

---

## 6. Extensibility

### 6.1 Custom Post Action

Use:

```c
int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx);
```

This allows custom parsing, aggregation, filtering, or external export logic.

### 6.2 Future Directions

- Add new opcode types and protocol versioning
- Introduce a dedicated deserialization layer and event bus
- Support configurable fault-tolerance policy (strict fail vs skip-bad-record)
- Add unit tests and replay tests using protocol fixtures

---

## 7. Architectural Constraints

- Current read path is single-threaded and synchronous
- Post action runs inline and can affect throughput
- Parsing logic should avoid heavy dynamic allocation
- Depends on Linux NVMe ioctl APIs and is platform specific

## 8. Current Parsing Fast Path Notes

- Record decode uses unaligned 64-bit little-endian loads (`load_le64_u`) and bit extraction.
- 8B records use one 64-bit load; 16B records use two 64-bit loads.
- Trim groups are parsed as one logical object, reducing repeated per-record orchestration overhead.

## 9. Read/Post-Action Pipeline

The current read path uses a producer-consumer pipeline to overlap I/O and parsing:

- Reader thread (producer):
  - reads NVMe chunks into a slot pool
  - enqueues ready slots
- Post-action thread (consumer):
  - dequeues ready slots
  - runs `nvme_post_action_process(...)`
  - returns slots to free queue

Implementation characteristics:

- Double-ended queues implemented with fixed-size ring buffers
- 4 in-flight slots (`NVME_READ_PIPELINE_SLOTS`)
- Thread synchronization via `pthread_mutex_t` + `pthread_cond_t`
- Unified error propagation (`producer_failed` / `worker_failed`), with coordinated stop

Benefit:

- Overlaps device read latency with CPU-side parsing to improve throughput on mixed I/O+CPU workloads.
