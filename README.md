# Context-Insight-Analyze

This project is an NVMe read-path tool based on Linux NVMe ioctl interfaces. It supports:

- Configurable `slba` and `data_len` arguments (with `K/M/G/T` unit suffixes)
- Dynamic MDTS probing to select a safe chunk size
- A pluggable post-action callback in the read path (default implementation includes structured record parsing)
- Post-action parsing without file output (read data is validated/parsed in-memory only)
- Trim group aggregation (`total_ranges > 1`) into a single object with `ranges[]`
- Early termination marker support: an all-zero 16-byte record (`0x00` * 16) is treated as end-of-valid data and triggers soft-stop

## Documentation

- Design document: `docs/design.md`
- Architecture document: `docs/architecture.md`
- Usage guide: `docs/usage.md`
