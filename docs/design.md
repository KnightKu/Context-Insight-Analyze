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
| Reserved | 1B | 1 | Must be `0x00` |
| QD | 2B | 2 | Queue depth |
| WA | 1B | 4 | Write amplification |
| Time | 3B | 5 | Timestamp |

### 3.4 Marker (`0xFF`) - 8B

| Field | Size | Offset | Description |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0xFF` |
| Absolute time | 7B | 1 | High-precision absolute timestamp |

## 4. Core Algorithm

### 4.1 Endianness Decoder

The parser uses `load_le_u64_n(const unsigned char *p, uint32_t nbytes)` for all packed fields:

- Accepts any little-endian fragment of 1..8 bytes
- Returns a `uint64_t`
- Reused by all 3B / 5B / 7B fields to avoid duplicated bit logic

### 4.2 Record Dispatch and Boundary Safety

`default_post_action` uses this flow:

1. Validate `data != NULL` and alignment constraints
2. Start scanning with `cursor = 0`
3. Read `op = bytes[cursor]`
4. Determine record size (8 or 16) from opcode
5. Fail with `truncated record` if remaining bytes are insufficient
6. Dispatch to one parser:
   - `parse_rw_record`
   - `parse_trim_record`
   - `parse_stat_record`
   - `parse_marker_record`
7. Move `cursor += record_size` and continue

### 4.3 Validation Rules

- Unknown opcode -> `EINVAL` with explicit log
- Non-zero reserved field -> `EINVAL` with explicit log
- Non-8-byte-aligned `data_len` -> fail fast to prevent record misalignment

## 5. Error Handling Strategy

- Parse failures return `-1` and set `errno` (typically `EINVAL`)
- `nvme_read` aborts when post action returns error
- Logs include `offset`, `record index`, `opcode`, and field context for troubleshooting

## 6. Debug and Observability

Compile-time macro `NVME_POST_ACTION_DEBUG` controls debug logs:

- Default: `0` (off)
- Set to `1` to print parsed fields for each record type
- When `data_len < 8`, the "no complete 8-byte unit" message is printed only in debug mode

## 7. Extensibility

To add a new opcode:

1. Add opcode constant
2. Implement parser function
3. Extend dispatch switch with record size and parser mapping

For future variable-length records, parse a minimal header first, then compute dynamic record size.

## 8. Known Limits and Recommendations

- Current implementation validates structure and reserved fields only; it does not enforce cross-record semantics (for example, Trim range sequence consistency).
- Recommended next steps:
  - Add unit tests per opcode (valid/invalid)
  - Add truncated/misaligned record tests
  - Add reserved-field non-zero tests
  - Add mixed-record stream tests
