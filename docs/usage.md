# Usage Guide

This document explains how to build and run `nvme_reader`, how to enable debug logging,
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
./nvme_reader
```

## 3. CLI Arguments

Current command format:

```bash
./nvme_reader <device_name> <slba[K|M|G|T]> <data_len[K|M|G|T]>
```

Argument details:

- `device_name`: device path, for example `/dev/nvme0n1`
- `slba`: start LBA, supports unit suffix `K/M/G/T` (case-insensitive)
- `data_len`: read size in bytes, supports unit suffix `K/M/G/T` (case-insensitive)

Examples:

```bash
./nvme_reader /dev/nvme0n1 0 64M
./nvme_reader /dev/nvme0n1 1G 256M
./nvme_reader /dev/nvme0n1 1024K 1T
```

Notes:

- Units use binary scaling (`1M = 1024 * 1024`).
- `data_len` must satisfy the runtime sector alignment check.

## 4. Default Post Action Behavior

Every read chunk is passed to the default post action for format parsing and validation.
See:

- `docs/design.md` (field-level protocol details)
- `docs/architecture.md` (module interactions and flow)

The default post action:

- Does not modify data
- Does not write additional files
- Focuses on format validation and observability

## 5. Debug Macro

File: `nvme_read.c`

```c
#ifndef NVME_POST_ACTION_DEBUG
#define NVME_POST_ACTION_DEBUG 0
#endif
```

Set it to `1` to enable debug logs:

- Prints parsed fields for each record type
- Prints a message when `data_len < 8` (no complete 8-byte record)

How to enable:

1. Change the macro value to `1`
2. Rebuild:

```bash
make clean && make
```

## 6. Troubleshooting

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

- Use a `data_len` value aligned to device sector size (typically 512 bytes)

### 6.3 Post Action Parse Errors

Typical errors:

- `post action invalid op`
- `post action truncated record`
- `post action invalid ... reserved`

Checks:

- Verify record layout against `docs/design.md`
- Verify opcode and record size mapping (8-byte vs 16-byte records)

## 7. Common Development Commands

```bash
# Build
make clean && make

# Current branch and working tree status
git branch --show-current
git status --short

# Push dev branch
git push -u origin dev
```
