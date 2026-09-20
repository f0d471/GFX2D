# SW3 命令表构造层与软件执行器

状态：未开工。前置 SW1（`sw/lib/` 与两套构建）和已定稿的
[SW2](../../TechReports/spec/02-命令语义.md)；不再实现可变的“默认答案”。
阻塞 SW6、SW7 的画面，以及 E7 的板上测量（E7 直接链接 `sw/lib/gfx/`）。

## 1 要解决什么

游戏只能通过命令表出画面（[总览](SW1-SW9总览.md)约束 4），所以需要两样东西：

- **`sw/lib/gfx/` 的构造接口**：游戏调用的构造接口，负责表面登记、按 3.4 节的位域打包 32 字节记录、命令表满时的处理。
  游戏代码里不出现位移与掩码。
- **`sw/lib/gfx/` 的软件执行器**：按 spec 第 01、02 章逐像素执行命令表。它有三个用途（设计文档 3.8 节）：
  PC 上的渲染器、板上软件渲染模式与对比基线、引擎仿真与上板 CRC 的金标准。

[E7](../hw/E7-软件执行器像素吞吐.md) 原计划在 `sw/apps/probe/execSwTest` 里写执行器本体。改为在这里写一份、
在 PC 上验证正确性，E7 只保留板上的用例、打点、三种编译配置与吞吐表，避免同一个金标准写两份。

## 2 怎么切分

### 2.1 构造接口（`sw/lib/gfx/list.c` 等）

```c
typedef struct { uint32_t w[8]; } gfx_cmd;           /* _Static_assert(sizeof == 32) */

int gfx_surface_set(gfx_surface_table *t, int id, const gfx_surface *s);
void gfx_list_begin(gfx_list *l, gfx_cmd *buf, uint32_t cap,
                    const gfx_surface_table *t);
int gfx_fill(gfx_list *l, int dst, rect r, uint32_t argb, int eq, uint8_t c);
int gfx_blit(gfx_list *l, int src, int sx, int sy, int dst, int dx, int dy,
             int w, int h, int eq, uint8_t c, int key_en, uint32_t key);
int gfx_set_clip(gfx_list *l, rect r);
int gfx_list_end(gfx_list *l);
```

- 位域用移位与掩码打包，**不用 C 的位域语法**（位域的排列由编译器决定）。
- w7 标签由构造层默认填为零基命令索引；错误结果仍分别保留索引与标签。
- `gfx_surface_set()` 显式接收表面表，不使用模块全局变量；它校验描述符结构并最后置 `valid`。
  `gfx_list_begin()` 把该表绑定到本次构造上下文，后续 FILL/BLIT 才能检查 space、format 与重叠。
- 表面格式显式区分 `XRGB8888`、`ARGB8888_PREMULT` 与 `ARGB8888_STRAIGHT`；
  构造层拒绝非 DDR、非 XRGB 目的、方程/源格式错配和同表面重叠。
- 所有追加函数返回 `int`：0 表示已追加，正值沿用 spec 错误码（非法调用不追加），
  `GFX_BUILD_FULL = -1`、`GFX_BUILD_CLOSED = -2`、`GFX_BUILD_BAD_ARG = -3` 是构造层错误。
  坐标必须能原样放进 int16，宽高必须能原样放进 uint16，表面号必须在 0~15；空指针、越界参数
  或其它不能无损编码的输入返回 `GFX_BUILD_BAD_ARG`，禁止先截断再打包。`gfx_list` 保存
  `first_build_error`、`rejected`、`overflow` 和 `ended`；即使上层暂不逐次检查返回值，帧末也能
  报出首错和计数，不能静默丢命令。被拒绝的调用不占记录索引，后续成功记录的 w7 仍等于其表内索引。
- 命令表满：不再追加，`overflow` 加一并返回 `GFX_BUILD_FULL`；END 后继续追加返回
  `GFX_BUILD_CLOSED`。COUNT 耗尽本来就是合法结束，因此“表满时没位置写 END”不会破坏已构造前缀。
- 构造时校验只是尽早报错，不替代执行器校验：表面表可能在构造后、执行前被改写，执行器仍按
  spec 完整重查原始记录与当时的表面表。
- 同时提供 `gfx_cmd_decode()`，调试时把命令打印成一行文本。

### 2.2 软件执行器（`sw/lib/gfx/exec_sw.c` 等）

```c
int exec_sw_run(const gfx_surface_table *t, const gfx_cmd *list,
                uint32_t list_count, uint32_t readable_count,
                exec_sw_stats *st, exec_sw_error *err);
/* 返回 spec 第 02 章的错误码；err 分别给出当前表内零基 index 与 tag */
```

- 结构照 spec：命令解释 → 裁剪（第 01 章第 5 条）→ 逐像素（色键判定、方程、舍入、写回）。
- `list_count` 是 `LIST_COUNT`，`readable_count` 是调用方保证可读的记录数；每次取记录前先检查
  `index < readable_count`，否则返回 `FETCH`、当前 `index` 和 `tag = 0`，绝不靠越界解引用模拟
  取数失败。构造层的正常提交令两者相等，短表向量故意令前者更大。
- 每个方程一个内联函数，外层按方程选一次，内层循环不按像素切换方程。
- 统计与硬件计数器同名同口径（设计文档 3.4 节末、4.6 节）：`CMD`、`PIXEL`、`KEY_SKIP`，
  另加 `pixel_total` 与 `pixel_written`（E7 2.4 节的口径）。
- 注错宏每个方程一个：`-DINJECT_EQ_COPY`、`-DINJECT_EQ_ALPHA` 等，只把该方程输出的最低位取反。

### 2.3 主机侧测试（`sw/lib/test/`）

| 组 | 内容 |
| --- | --- |
| 编解码 | 随机生成 10 万条命令，打包再解包字段逐一相等 |
| 构造失败 | 两张独立表面表交叉构造，证明无全局串扰；逐项触发 spec 错误、不可编码参数、表满和 END 后追加，断言返回值、`first_build_error`、计数与 `used`，被拒绝调用不改变缓冲区也不占 w7 索引 |
| 单像素 | 每个方程取 spec 第 01 章给出的反例与边界值（`c = 1, src = 1, dst = 128` 等），输出与手算一致 |
| 裁剪 | 负坐标、部分出屏、宽高为 0、`SET_CLIP` 空矩形与跨帧继承，逐条对应第 01 章第 5 条 |
| 命令语义 | 直接使用 spec 第 02 章规范条款 §7 的版本化向量（ABI 1）：`tools/check/check_cmd_semantics.py --emit` 出 159 条；`[list]` 段保留多次提交边界及各自的 `count`/可读记录数，覆盖 opcode/方程正例、逐字段反例、格式错配、重叠边界、COUNT/END/FETCH、错误优先级与无副作用。**另写一份从第 02 章独立生成的向量**与它对照——工具那份是第三份实现，用来发现「两份实现共享同一个错误」 |
| 整帧 | E7 2.2 节的九个用例在 PC 上各跑一遍，输出整帧 CRC32，写进 `sw/lib/test/golden.txt`，E7 与引擎仿真都拿它当期望值 |
| 独立实现 | 另写一个不追求速度、逐像素直接照伪代码抄的参考版 `exec_ref`；源码随 `sw/lib/test/` 提交但不进入产品库目标，整帧用例两者 CRC 必须一致 |

独立实现这一组是针对 spec 第 01 章复核时发现的问题加的：工具自己判自己「全部通过」，
没有第二个实现做对照，模型写错了也看不出来。

本条**不含**：板上打点与吞吐（E7）、条带分桶（M4）、SDL 显示（SW4）。

## 3 判据

| 项 | 判据 |
| --- | --- |
| 单元测试 | `sw/lib/test/` 在 64 位与 `-m32` 下全部通过且输出相同，开 ASan 与 UBSan 无报告 |
| 构造接口 | 无模块全局表面状态；非法调用/表满/已结束三类均返回非零且不写缓冲区，sticky 首错与计数可在帧末读取 |
| 两个实现一致 | 整帧九个用例 `exec_sw` 与 `exec_ref` 的 CRC32 一致 |
| ABI 一致 | spec 第 02 章全部命令向量在 64 位、`-m32` 与 `exec_ref` 上的返回码、index、tag、状态与像素逐项一致 |
| 注错见红 | 逐个打开每个方程的注错宏：用到该方程的用例 CRC 变，其余不变。用到 ALPHA 的用例表与不用的用例表写在开发日志里，逐方程各验一次 |
| 跨平台 | 同一份 `sw/lib/gfx/` 用 xPack gcc 8.3.0 编过；E7 开工时板上九个用例的 CRC 与 `sw/lib/test/golden.txt` 一致（这一条在 E7 里验） |
| 统计口径 | `pixel_written + KEY_SKIP + SRC_OOB`（源越界跳过的像素，第 01 章第 5 条第 5 步）等于裁剪后目的矩形的像素数，每个用例对账成立 |

## 4 开发日志

（未开工）
