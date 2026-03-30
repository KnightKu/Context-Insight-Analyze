# Context-Insight-Analyze

This project is an NVMe read-path tool based on Linux NVMe ioctl interfaces. It supports:

- Configurable `slba` and `data_len` arguments (with `K/M/G/T` unit suffixes)
- Dynamic MDTS probing to select a safe chunk size
- A pluggable post-action callback in the read path (default implementation includes structured record parsing)

## Documentation

- Design document: `docs/design.md`
- Architecture document: `docs/architecture.md`
- Usage guide: `docs/usage.md`
