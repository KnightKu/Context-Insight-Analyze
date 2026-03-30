# NVMe Reader Post Action 设计文档

## 1. 背景与目标

本项目通过 NVMe passthru 读取设备数据，并在每次读取后执行 `post action` 对数据流进行解析与校验。  
本文档聚焦 `post action` 子系统的设计，目标包括：

- 按协议解析多种记录格式（8B/16B）
- 以小端方式精确还原字段
- 在数据不完整、字段非法时快速失败并返回可定位错误
- 提供可控调试输出，不影响默认性能路径

## 2. 设计约束

- 读取路径是性能敏感路径，默认解析应保持轻量。
- `post action` 在 `nvme_read` 的循环中同步执行，返回错误会中断读取。
- 输入是裸字节流，必须通过长度和 opcode 双重约束决定记录边界。
- 字段使用非标准宽度（如 3B/5B/7B），必须统一使用小端拼接逻辑。

## 3. 数据协议与记录定义

当前支持 5 种 opcode，分为两类记录长度：

- 16B 记录：`0x01` (Read), `0x02` (Write), `0x03` (Trim)
- 8B 记录：`0x0F` (Stat), `0xFF` (Marker)

### 3.1 Read / Write (`0x01` / `0x02`) - 16B

| 字段 | 字节宽度 | 偏移 | 说明 |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0x01` 读，`0x02` 写 |
| Start LBA | 5B | 1 | 小端，解析到 `uint64_t` 并掩码高位 |
| Length | 2B | 6 | 请求数据长度 |
| Reserved | 2B | 8 | 固定 `0x0000` |
| Latency | 3B | 10 | 命令延迟 |
| Time | 3B | 13 | 相对时间戳 |

### 3.2 Trim (`0x03`) - 16B

| 字段 | 字节宽度 | 偏移 | 说明 |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0x03` |
| Start LBA | 5B | 1 | 当前 Range 起始地址 |
| Total ranges | 1B | 6 | 本次 Trim 的总区间数（协议定义最大 256） |
| Range index | 1B | 7 | 当前区间索引 |
| Reserved | 1B | 8 | 固定 `0x00` |
| Length | 4B | 9 | 当前 Range 长度 |
| Time | 3B | 13 | 时间戳 |

> 说明：当 `Total ranges > 1`，多个连续 Trim 记录共同构成一个逻辑 Trim 任务。

### 3.3 Stat (`0x0F`) - 8B

| 字段 | 字节宽度 | 偏移 | 说明 |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0x0F` |
| Reserved | 1B | 1 | 固定 `0x00` |
| QD | 2B | 2 | 队列深度 |
| WA | 1B | 4 | 写放大系数 |
| Time | 3B | 5 | 时间戳 |

### 3.4 Marker (`0xFF`) - 8B

| 字段 | 字节宽度 | 偏移 | 说明 |
|---|---:|---:|---|
| Opcode | 1B | 0 | `0xFF` |
| Absolute time | 7B | 1 | 全局绝对时间戳 |

## 4. 核心算法设计

### 4.1 字节序解析

统一使用 `load_le_u64_n(const unsigned char *p, uint32_t nbytes)`：

- 输入任意 `nbytes`（1~8）的小端字节片段
- 返回 `uint64_t`
- 所有 3B/5B/7B 字段都复用该函数，避免重复位运算逻辑

### 4.2 记录分派与边界控制

在 `default_post_action` 中：

1. 先做前置校验：`data != NULL`、`data_len` 对齐校验
2. `cursor` 从 0 开始扫描数据块
3. 读取 `op = bytes[cursor]`
4. 根据 opcode 选择记录长度（8 或 16）
5. 若剩余长度不足记录长度，报 `truncated record` 并失败
6. 调用对应解析函数（`parse_rw_record/parse_trim_record/parse_stat_record/parse_marker_record`）
7. `cursor += record_size`，继续下一条记录

### 4.3 字段合法性校验

- 对未知 opcode：`EINVAL` + 明确错误日志
- 对 `Reserved` 字段非 0：`EINVAL` + 明确错误日志
- 对非 8B 对齐 `data_len`：直接失败，防止跨记录误读

## 5. 错误处理策略

- 解析失败统一返回 `-1`，并设置 `errno`（通常为 `EINVAL`）
- `nvme_read` 在收到 post action 失败后会中断读流程并返回错误
- 日志包含：`offset`、`record index`、`opcode`、必要字段值，便于定位

## 6. 调试与可观测性

通过编译宏 `NVME_POST_ACTION_DEBUG` 控制调试日志：

- 默认值：`0`（关闭）
- 置为 `1`：输出每类记录的解析字段
- 当 `data_len < 8`（无完整 8B 单元）时，仅 debug 打开时打印提示

## 7. 扩展性设计

- 新增 opcode 时，只需：
  1. 新增 opcode 常量
  2. 新增解析函数
  3. 在分派 `switch` 中映射记录长度与解析函数
- 若未来存在变长记录，可在分派阶段先解析最小头，再动态计算记录长度。

## 8. 已知边界与后续建议

- 当前仅做结构级与保留字段校验，不做跨记录语义关联校验（如 Trim 多 range 完整性）。
- 建议后续补充单元测试：
  - 各 opcode 正例/反例
  - 截断记录与错位对齐
  - reserved 非零异常路径
  - 多记录混合解析路径
