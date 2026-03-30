# 使用文档（Usage Guide）

本文档说明 `nvme_reader` 的编译、运行参数、调试开关以及常见问题定位方式。

## 1. 环境要求

- Linux 环境（依赖 NVMe ioctl 接口）
- C 编译器（如 `gcc`）
- 需要对目标 NVMe 设备有读取权限

## 2. 编译

在仓库根目录执行：

```bash
make clean && make
```

生成可执行文件：`./nvme_reader`

## 3. 命令行参数

当前程序参数格式：

```bash
./nvme_reader <device_name> <slba[K|M|G|T]> <data_len[K|M|G|T]>
```

参数说明：

- `device_name`：设备路径，例如 `/dev/nvme0n1`
- `slba`：起始 LBA，可带单位 `K/M/G/T`（大小写均可）
- `data_len`：读取字节数，可带单位 `K/M/G/T`（大小写均可）

示例：

```bash
./nvme_reader /dev/nvme0n1 0 64M
./nvme_reader /dev/nvme0n1 1G 256M
./nvme_reader /dev/nvme0n1 1024K 1T
```

说明：

- 单位按 `1024` 进制换算（例如 `1M = 1024 * 1024`）。
- `data_len` 需要满足设备扇区大小对齐（程序运行时会校验）。

## 4. 默认 post action 行为

读取到的数据会进入默认 post action 做格式解析与校验，解析规则见：

- `docs/design.md`（字段级定义）
- `docs/architecture.md`（流程与模块关系）

默认 post action：

- 不修改读取数据
- 不写额外输出文件
- 主要用于数据格式校验和调试观测

## 5. Debug 宏

文件：`nvme_read.c`

```c
#ifndef NVME_POST_ACTION_DEBUG
#define NVME_POST_ACTION_DEBUG 0
#endif
```

将其改为 `1` 可启用调试日志：

- 输出各记录解析后的关键字段
- 当 `data_len < 8`（没有完整 8B 单元）时打印提示

开启方式示例：

1. 修改宏值为 `1`
2. 重新编译：

```bash
make clean && make
```

## 6. 错误定位建议

### 6.1 参数错误

常见表现：

- `invalid slba`
- `invalid data_len`

排查建议：

- 检查单位是否为 `K/M/G/T`
- 检查输入是否为非负整数 + 可选单位后缀

### 6.2 对齐错误

常见表现：

- `data_len must be ...-byte aligned`
- `read chunk must be ...-byte aligned`

排查建议：

- 将 `data_len` 调整为扇区大小（通常 512）整数倍

### 6.3 post action 格式错误

常见表现：

- `post action invalid op`
- `post action truncated record`
- `post action invalid ... reserved`

排查建议：

- 对照 `docs/design.md` 确认记录长度和字段布局
- 确认 opcode 与对应结构长度（8B / 16B）匹配

## 7. 常用开发命令

```bash
# 构建
make clean && make

# 查看当前分支和状态
git branch --show-current
git status --short

# 推送当前分支
git push -u origin dev
```
