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

- Default post action:
  - Record parsing and format validation
  - Optional debug logs (`NVME_POST_ACTION_DEBUG`)

- Main read function:
  - `nvme_read(...)`

---

## 3. Execution Sequence

Main read sequence:

1. `main` calls `nvme_read`
2. Open NVMe device node
3. Detect logical sector size (`BLKSSZGET`)
4. Query MDTS and determine chunk size
5. Submit `NVME_IOCTL_IO_CMD` in a loop
6. Run `g_post_action(...)` after each chunk
7. Print statistics and release resources

---

## 4. Data Flow

1. Input argument flow
   - CLI string arguments -> numeric values (`uint64_t`)

2. Device data flow
   - Device -> `chunk_buf` (memory)
   - `chunk_buf` -> post action (parse/validate)

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
