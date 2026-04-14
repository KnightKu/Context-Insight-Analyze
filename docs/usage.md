# Usage Guide

This document explains how to build and run `sfx_ctx_insight_analyze`, how to enable debug logging,
and how to troubleshoot common runtime errors.

## 1. Requirements

- Linux (depends on NVMe ioctl interfaces)
- C compiler (for example, `gcc`)
- Read permission for the target NVMe device node

## 2. Build

Run in the repository root:

```bash
make clean && make
```

Binary output:

```bash
./sfx_ctx_insight_analyze
```

## 3. CLI Arguments

Current command format:

```bash
./sfx_ctx_insight_analyze [-D|--debug] [-l|--latency] [-r|--read-count] [-w|--w2fr] [-c|--life-cycle] [-q|--qd-dist] [-a|--wa-dist] [-R|--read-size-dist] [-W|--write-size-dist] [-T|--trim-size-dist] <device_name> <slba[K|M|G|T]> <data_len[K|M|G|T]>
```

Argument details:

- `device_name`: device path, for example `/dev/nvme0n1`
- `slba`: start LBA, supports unit suffix `K/M/G/T` (case-insensitive)
- `data_len`: read size in bytes, supports unit suffix `K/M/G/T` (case-insensitive)
- `-D` / `--debug`: enables runtime debug mode
- `-l` / `--latency`: enables fio-style latency summary output for post-action `read/write/trim`
- `-r` / `--read-count`: enables `Read Count` distribution output
- `-w` / `--w2fr`: enables `Write-to-First-Read Latency` real-time(ms) distribution output
- `-c` / `--life-cycle`: enables `LBA Life Cycle` real-time(ms) distribution output
- `-q` / `--qd-dist`: enables `QD` distribution output
- `-a` / `--wa-dist`: enables `WA` (write amplification) distribution output
- `-R` / `--read-size-dist`: enables read size distribution output (4K block units)
- `-W` / `--write-size-dist`: enables write size distribution output (4K block units)
- `-T` / `--trim-size-dist`: enables trim size distribution output (4K block units)

Examples:

```bash
./sfx_ctx_insight_analyze /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze /dev/nvme0n1 1G 256M
./sfx_ctx_insight_analyze /dev/nvme0n1 1024K 1T
./sfx_ctx_insight_analyze --debug /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze --latency /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze -D -l /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze --read-count /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze --w2fr /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze --life-cycle /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze -D -r -w -c /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze --qd-dist --wa-dist /dev/nvme0n1 0 64M
./sfx_ctx_insight_analyze --read-size-dist --write-size-dist --trim-size-dist /dev/nvme0n1 0 64M
```

Notes:

- Units use binary scaling (`1M = 1024 * 1024`).
- `sector_size` is fixed to `4096` bytes (4KiB).
- `data_len` and `slba` must both be 4KiB-aligned.
- Runtime log range constants (byte-space):
  - `LOG_START_LBA = 937684566 * sector_size`
  - `LOG_END_LBA = 3750730325 * sector_size`
- Effective backup address check is done in byte-space:
  - `backup_lba_bytes = LOG_START_LBA + slba + offset_bytes`
  - must satisfy `backup_lba_bytes < LOG_END_LBA`
  - then converted to sector-space for `cdw14/cdw15` via `/ sector_size`
- `slba` is converted by `slba / 4096` before being used in NVMe command LBA fields.
- Read chunk uses device MDTS-derived size with an internal cap of `mdts <= 6`
  (that is, chunk up to `1 << (12+6) = 256KiB`).

## 4. Default Post Action Behavior

Every read chunk is passed to the default post action for format parsing and validation.
See:

- `docs/design.md` (field-level protocol details)
- `docs/architecture.md` (module interactions and flow)

The default post action:

- Does not modify data
- Does not write additional files
- Focuses on format validation and observability
- Parses records using a fast 64-bit load path (`load_le64_u`)
- Aggregates consecutive Trim ranges into a single logical Trim group object
- Treats marker (`0xFF`) as a 16-byte record:
  - low 8 bytes: opcode + `abs_time` (7B, microseconds), same as previous logic
  - high 8 bytes: first byte is reserved and must be `0x00`, remaining 7 bytes are Unix timestamp in milliseconds (`unix_time_ms`)
- Interprets `time` fields in `read/write/trim/stat` as relative offsets (3B) from the latest previous marker
- Records before the first marker are treated as invalid preamble/noise and dropped
- Keeps marker time reference continuous across read chunks (stream-level marker continuity)
- Requires marker timestamps to be strictly increasing:
  - if marker `abs_time` is non-increasing (`<=` previous marker), it is treated as overwrite
  - post-action enters soft-stop immediately: stop further log read/parse, but main flow keeps success path
  - already-processed records remain valid, and subsequent non-read operations continue
  - warning log includes overwrite context and LBA location (`lba`, `offset`, marker timestamps)
- Supports early full-zero termination:
  - if a 16-byte record is all zero (`00 * 16`), parser treats it as end-of-valid-log
  - post-action soft-stops immediately (no further parse in current/next chunks)
  - read pipeline also stops further device reads, while main flow still returns success
  - non-zero `op == 0x00` records are still treated as invalid/noise and skipped by 16 bytes
- Maintains in-memory per-4KB LBA statistics for runtime analysis:
  - 1B saturated read count (`0..255`) for write-then-read events
  - 2B non-linear encoded write-to-first-read latency
  - 2B non-linear encoded write life-cycle latency (write to next overwrite)
  - LBA and length units are sectors; sector size is fixed to `4096B (4KiB)`
  - Advanced life-cycle overwrite statistics:
    - Tracks overwrite-write segments by size bins: `4K, 8K, 16K, ...` up to MDTS-aligned cap
    - Requires all covered 4K buckets in a segment to share the same life-cycle code
    - Uses the same non-linear life-cycle code as grouping key
    - Non-4K-aligned overwrite lengths are rounded down to 4K powers (example: `12K -> 8K`)
- Optional latency summary (`-l/--latency`) for post-action records:
  - `read`, `write`: use protocol `latency` field (3B)
  - `trim`: use per-range relative `time` delta as trim latency sample
  - Output includes `count/min/max/avg`
  - Percentiles cover `p10`, `p20`, `p30`, `...`, `p90`, `p99`, `p99.9`, `p99.99`
- Optional LBA stats summary (independent switches):
  - `-r` / `--read-count`: `Read Count` non-zero bucket distribution
  - `-w` / `--w2fr`: `Write-to-First-Read Latency` decoded real-time distribution (milliseconds)
  - `-c` / `--life-cycle`: `LBA Life Cycle` decoded real-time distribution (milliseconds)
  - Output format is segmented ratio buckets, similar to `-l/--latency` style
- Optional workload distribution summary (independent switches):
  - `-q` / `--qd-dist`: `QD` distribution from Stat records
  - `-a` / `--wa-dist`: `WA` distribution from Stat records (`(hot_write + folding_write) / hot_write`)
    - 10 fixed buckets compressed to range `1.0~5.0`: `[1.0,1.4) ... [4.6,5.0]`
  - `-R` / `--read-size-dist`: read size distribution (`length` field, 4K units)
  - `-W` / `--write-size-dist`: write size distribution (`length` field, 4K units)
  - `-T` / `--trim-size-dist`: trim size distribution (per-range `length`, 4K units)

## 5. Read/Process Pipeline

The main read path uses a producer-consumer pipeline to overlap device reads and post-action processing:

- **Reader thread (producer)**:
  - issues NVMe read commands
  - writes chunks into ring-buffer slots
- **Worker thread (consumer)**:
  - consumes ready slots
  - runs `nvme_post_action_process(...)`

Current default settings:

- ring-buffer slots: `4` (`NVME_READ_PIPELINE_SLOTS`)
- one reader thread + one worker thread
- stats bucket size: `4KB` per bucket
- stats storage: `5 bytes` per bucket (`ReadCount[1] + W2FR[2] + LifeCycle[2]`)
- default total LBA span: `4TB` (`~1,073,741,824` buckets, ~`5GB` virtual memory)

This design helps reduce idle time by allowing I/O and parsing to run concurrently.
- Prints read bandwidth statistics only when debug mode is enabled (`-D` / `--debug`)
- In debug mode, prints post-action stats summary (`buckets`, `touched`, `read_count_nonzero`, `bytes`)
- Prints fio-style latency summary only when latency mode is enabled (`-l` / `--latency`)
- Prints each LBA stats segmented ratio section only when the corresponding `-r/-w/-c` switch is enabled

## 6. Debug Macro

Files:

- `post_action.c` (parser/debug macros)
- `post_action_stats.c` (statistics backend)

```c
#ifndef NVME_POST_ACTION_DEBUG
#define NVME_POST_ACTION_DEBUG 0
#endif
```

Optional stat-skip macro:

```c
#ifndef NVME_POST_ACTION_SKIP_STAT
#define NVME_POST_ACTION_SKIP_STAT 0
#endif
```

When `NVME_POST_ACTION_SKIP_STAT` is set to `1`, post action still consumes 16-byte
`Stat (0x0F)` records to keep stream alignment, but skips stat field parsing/validation
and directly continues with the next record.

Set `NVME_POST_ACTION_DEBUG` to `1` to enable debug logs:

- Prints parsed fields for each record type
- Prints a message when `data_len < 16` (no complete 16-byte record)

How to enable:

1. Change the macro value to `1`
2. Rebuild:

```bash
make clean && make
```

## 7. Troubleshooting

### 6.1 Invalid CLI Arguments

Typical errors:

- `invalid slba`
- `invalid data_len`

Checks:

- Confirm suffix is one of `K/M/G/T`
- Confirm input is a non-negative integer with optional unit suffix

### 6.2 Alignment Errors

Typical errors:

- `data_len must be ...-byte aligned`
- `read chunk must be ...-byte aligned`

Checks:

- Use a `data_len` value aligned to `4096` bytes (fixed sector size)

### 6.3 Post Action Parse Errors

Typical errors:

- `post action invalid op`
- `post action truncated record`
- `post action invalid ... reserved`
- `post action trim group broken`
- `post action trim group inconsistent total_ranges`

Checks:

- Verify record layout against `docs/design.md`
- Verify opcode and record size mapping:
  - protocol records (`read/write/trim/stat/marker`) are 16-byte
  - parser control/noise skip unit is also 16-byte (`op == 0x00` / unknown opcode)

## 8. Common Development Commands

```bash
# Build
make clean && make

# Run post-action tests (file-input based)
make test

# Current branch and working tree status
git branch --show-current
git status --short

# Push dev branch
git push -u origin dev
```

## 9. Post-Action Test Program

The repository includes a dedicated test binary that feeds file data into the post-action interface:

```bash
./post_action_file_tester [-D|--debug] [-l|--latency] [-r|--read-count] [-w|--w2fr] [-c|--life-cycle] [-q|--qd-dist] [-a|--wa-dist] [-R|--read-size-dist] [-W|--write-size-dist] [-T|--trim-size-dist] <input_file> [offset_bytes]
```

Behavior:

- Reads all bytes from `input_file`
- Calls `nvme_post_action_process(data, data_len, offset_bytes)`
- Supports the same option set as the main binary:
  - `-D` / `--debug`
  - `-l` / `--latency`
  - `-r` / `--read-count`
  - `-w` / `--w2fr`
  - `-c` / `--life-cycle`
  - `-q` / `--qd-dist`
  - `-a` / `--wa-dist`
  - `-R` / `--read-size-dist`
  - `-W` / `--write-size-dist`
  - `-T` / `--trim-size-dist`
- The only behavior difference from the main binary:
  - data source is `input_file` instead of NVMe device read
- Returns:
  - `0` on success
  - `1` on post-action validation failure
  - `2` on test-program usage or I/O error

Fixtures are generated by:

```bash
python3 tests/gen_post_action_fixtures.py
```
