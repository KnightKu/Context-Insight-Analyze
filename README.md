# Context-Insight-Analyze

本项目是一个基于 Linux NVMe ioctl 的读路径工具，支持：
- 指定 `slba` 与 `data_len`（支持 `K/M/G/T` 单位）
- 动态探测 MDTS 作为分片读大小
- 在读取路径上执行可插拔 post action（默认实现含结构化记录解析）

## 文档导航

- 设计文档：`docs/design.md`
- 架构文档：`docs/architecture.md`
- 使用文档：`docs/usage.md`
