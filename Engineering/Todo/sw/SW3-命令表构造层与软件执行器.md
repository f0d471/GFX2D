# SW3 命令表构造层与软件执行器

状态：未开工。前置 SW1（`sw/lib/` 与两套构建）；SW2 未定稿的条目先按其「建议的默认答案」实现，定稿后改。
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

void gfx_surface_set(int id, const gfx_surface *s);  /* 表面表，最多 16 个 */
void gfx_list_begin(gfx_list *l, gfx_cmd *buf, uint32_t cap);
void gfx_fill(gfx_list *l, int dst, rect r, uint32_t argb, int eq, uint8_t c);
void gfx_blit(gfx_list *l, int src, int sx, int sy, int dst, int dx, int dy,
              int w, int h, int eq, uint8_t c, int key_en, uint32_t key);
void gfx_set_clip(gfx_list *l, rect r);
void gfx_list_end(gfx_list *l);
```

- 位域用移位与掩码打包，**不用 C 的位域语法**（位域的排列由编译器决定）。
- w7 序号由构造层自动递增。
- 命令表满：不再追加，计数 `overflow` 加一，帧末由统计层报告。不静默截断。
- 同时提供 `gfx_cmd_decode()`，调试时把命令打印成一行文本。

### 2.2 软件执行器（`sw/lib/gfx/exec_sw.c` 等）

```c
int exec_sw_run(const gfx_surface_table *t, const gfx_cmd *list, uint32_t count,
                exec_sw_stats *st);                 /* 返回 0 或 spec 第 02 章第 9 条的错误码 */
```

- 结构照 spec：命令解释 → 裁剪（第 01 章第 5 条）→ 逐像素（色键判定、方程、舍入、写回）。
- 每个方程一个内联函数，外层按方程选一次，内层循环不按像素切换方程。
- 统计与硬件计数器同名同口径（设计文档 3.4 节末、4.6 节）：`CMD`、`PIXEL`、`KEY_SKIP`，
  另加 `pixel_total` 与 `pixel_written`（E7 2.4 节的口径）。
- 注错宏每个方程一个：`-DINJECT_EQ_COPY`、`-DINJECT_EQ_ALPHA` 等，只把该方程输出的最低位取反。

### 2.3 主机侧测试（`sw/lib/test/`）

| 组 | 内容 |
| --- | --- |
| 编解码 | 随机生成 10 万条命令，打包再解包字段逐一相等 |
| 单像素 | 每个方程取 spec 第 01 章给出的反例与边界值（`c = 1, src = 1, dst = 128` 等），输出与手算一致 |
| 裁剪 | 负坐标、部分出屏、宽高为 0、`SET_CLIP` 空矩形与跨帧继承，逐条对应第 01 章第 5 条 |
| 命令语义 | SW2 九条，每条至少一个用例 |
| 整帧 | E7 2.2 节的九个用例在 PC 上各跑一遍，输出整帧 CRC32，写进 `sw/lib/test/golden.txt`，E7 与引擎仿真都拿它当期望值 |
| 独立实现 | 另写一个不追求速度、逐像素直接照伪代码抄的参考版 `exec_ref`（只放在 `sw/lib/test/`，不进库），整帧用例两者 CRC 必须一致 |

独立实现这一组是针对 spec 第 01 章复核时发现的问题加的：工具自己判自己「全部通过」，
没有第二个实现做对照，模型写错了也看不出来。

本条**不含**：板上打点与吞吐（E7）、条带分桶（M4）、SDL 显示（SW4）。

## 3 判据

| 项 | 判据 |
| --- | --- |
| 单元测试 | `sw/lib/test/` 在 64 位与 `-m32` 下全部通过且输出相同，开 ASan 与 UBSan 无报告 |
| 两个实现一致 | 整帧九个用例 `exec_sw` 与 `exec_ref` 的 CRC32 一致 |
| 注错见红 | 逐个打开每个方程的注错宏：用到该方程的用例 CRC 变，其余不变。用到 ALPHA 的用例表与不用的用例表写在开发日志里，逐方程各验一次 |
| 跨平台 | 同一份 `sw/lib/gfx/` 用 xPack gcc 8.3.0 编过；E7 开工时板上九个用例的 CRC 与 `sw/lib/test/golden.txt` 一致（这一条在 E7 里验） |
| 统计口径 | `pixel_written + KEY_SKIP + SRC_OOB`（源越界跳过的像素，第 01 章第 5 条第 5 步）等于裁剪后目的矩形的像素数，每个用例对账成立 |

## 4 开发日志

（未开工）
