# Context Insight API

C library for querying NVMe insight logs: latency percentiles, workload distributions, write amplification, and related statistics. Output is JSON suitable for downstream parsing.

## Layout

| Path | Purpose |
|------|---------|
| `include/insight_api.h` | Public API |
| `src/` | Library implementation (NVMe read path, post-action parsers, metalog, JSON helpers) |
| `examples/` | Example programs (time-window and session-id query modes) |
| `tests/` | API validation tests |
| `docs/insight_api.md` | API reference |

## Build

```bash
make clean && make
```

Artifacts:

- `libinsight_api.a` — static library
- `libinsight_api.so` — shared library
- `insight_api_example` — time-window queries
- `insight_api_session_example` — session-id queries

Link your program with `-L. -linsight_api -lpthread -lm` and `-Iinclude`.

## Tests

```bash
make test
```

## Examples

Time window (caller supplies start/end and optional LBA range):

```bash
sudo ./insight_api_example /dev/nvme0n1 "2026-05-19 17:50:23" "2026-05-19 17:50:28" 4096
```

Session id (time/LBA resolved inside the library via metalog):

```bash
sudo ./insight_api_session_example /dev/nvme0n1 42 4096
```

See `docs/insight_api.md` for full interface documentation.
