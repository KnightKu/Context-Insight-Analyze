# NVMe Reader 架构文档

## 1. 总体架构

项目采用分层结构：

1. **CLI 层（`main.c`）**
   - 负责参数解析（`device_name`、`slba`、`data_len`）
   - 支持 `K/M/G/T` 单位参数
   - 调用核心库接口 `nvme_read(...)`

2. **核心读流程层（`nvme_read.c`）**
   - 设备打开与参数校验
   - 设备能力探测（扇区大小、MDTS）
   - 分块读取循环（NVMe passthru）
   - 调用 post action 处理读取到的数据
   - 统计吞吐并输出读性能

3. **扩展处理层（post action）**
   - 通过函数指针 `nvme_read_post_action_t` 提供扩展点
   - 默认实现按协议解析记录并做格式校验
   - 支持外部注入自定义回调

---

## 2. 代码模块与职责

### 2.1 `main.c`

- `parse_u64_with_unit(...)`
  - 将 `10K/64M/1G/1T` 等参数解析为字节数（或数值）
  - 处理溢出与非法后缀

- `main(...)`
  - 校验参数个数
  - 解析 `slba`、`data_len`
  - 调用 `nvme_read(...)`

### 2.2 `nvme_read.h`

- 常量定义：
  - `NVME_READ_CHUNK_BYTES`
  - `NVME_SPLIT_BYTES`
  - `NVME_DEFAULT_DATA_LEN`
  - `NVME_LBA_SIZE_BYTES`

- 类型定义：
  - `nvme_read_post_action_t`

- 对外接口：
  - `nvme_read_set_post_action(...)`
  - `nvme_read(...)`

### 2.3 `nvme_read.c`

- 能力探测函数：
  - `get_sector_size_or_default(...)`
  - `get_mdts_chunk_bytes_or_default(...)`

- 默认 post action：
  - 记录解析与格式校验
  - 调试日志打印（`NVME_POST_ACTION_DEBUG`）

- 主读流程：
  - `nvme_read(...)`

---

## 3. 关键时序

读流程主时序如下：

1. `main` 调用 `nvme_read`
2. 打开 NVMe 设备节点
3. 探测扇区大小（`BLKSSZGET`）
4. 查询 MDTS，决定单次读取 chunk 大小
5. 循环发起 `NVME_IOCTL_IO_CMD`
6. 每次读取后调用 `g_post_action(...)`
7. 所有 chunk 完成后输出统计信息并释放资源

---

## 4. 数据流

1. 参数输入流（用户输入）
   - CLI 文本参数 -> 数值（`uint64_t`）

2. 设备数据流（NVMe 读取）
   - 设备 -> `chunk_buf`（内存）
   - `chunk_buf` -> post action（解析/校验）

3. 诊断输出流
   - 错误输出到 `stderr`
   - 调试日志受宏控制

---

## 5. 错误处理策略

### 5.1 统一策略

- 失败时设置 `errno`（如 `EINVAL`、`EIO`）
- 输出定位信息（offset、record、op）
- 及时释放资源（buffer、fd）

### 5.2 输入与结构校验

- `data_len` 对齐要求
- post action 记录长度完整性
- reserved 字段必须为 0
- opcode 取值白名单

---

## 6. 可扩展性

### 6.1 自定义 post action

通过：

```c
int nvme_read_set_post_action(nvme_read_post_action_t action, void *ctx);
```

可注入业务解析、统计、过滤、写入外部系统等逻辑。

### 6.2 未来扩展方向

- 新增 opcode 类型与版本化协议
- 增加结构体反序列化层和统一事件总线
- 支持可配置的错误容忍策略（跳过坏包/严格失败）
- 增加单元测试与回放测试（record fixture）

---

## 7. 架构约束

- 读流程当前为单线程串行
- post action 同步执行，回调耗时会影响吞吐
- 解析逻辑默认在读取路径内，不应引入大块堆分配
- 依赖 Linux NVMe ioctl 接口，平台相关性较高
