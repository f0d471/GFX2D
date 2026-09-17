# sw：本项目编写的 RISC-V 程序

与 `sdk/` 的分工：`sdk/` 是 IP 生成的 BSP 和官方示例，重新生成 SoC IP 时会被覆盖；本目录放本项目编写的程序，不会被生成器碰到。

程序仍然沿用官方的 makefile 体系，每个程序的 makefile 里用 `STANDALONE` 指到 `sdk/software/standalone`，BSP 路径由 `bsp.mk` 从中推出。

## 目录

| 目录 | 说明 |
| --- | --- |
| `bootSizeTest/` | 检查 bootloader 从 Flash 拷贝的长度。带一个 300,000 字节、内容随位置变化的数据块，启动后打印整体与最后 4KB 的 CRC32 |
| `ddrAliasTest/` | 实验 E2：逐位翻转地址检查 DDR 地址译码与回绕，并演示数据缓存遮蔽别名的现象 |
| `ddrBurstProbe/` | 实验 E4：经 APB 驱动 `axi_probe_master`，做 CPU 与第二个主机的交叉读、突发长度扫描、并发实验 |
| `cacheCoherencyTest/` | 实验 E3：用探针主机当外部写入者，一致性判据、注错与注错自检，并量失效指令的代价 |
| `cacheMechTest/` | 实验 E3 的机理实验 T1~T11：装行规则、写缓冲、缓存行几何、失效时机、被跳过的失效指令、失效与装行的代价 |
| `cacheMinTest/` | 实验 E3 早期的最小实验，只作现象可复现保留，不作为结论证据 |

带 `logs/` 目录的程序，里面是与当前产物对应的上板原始日志（入库），文件名与 TechReport 里引用的一致。

## 编译

IDE 2026.1 自带的 gcc 13.4 编不了 2025.2 生成的这套 BSP（缺 `_zicsr`），当前用 xPack 的 gcc 8.3.0，安装见 `../doc/开发环境.md` 第 2 节：

```
wsl -e bash -c 'cd sw/bootSizeTest && make BSP=efinix/EfxSapphireSoc RISCV_BIN=<RISCV_BIN> OBJDIR=build'
```

`<RISCV_BIN>` 是工具链前缀，取值见 `../doc/开发环境.md` 第 1、11 节。

- 必须写 `wsl -e`，否则命令会先经过一层 shell，`$` 开头的变量被提前展开成空，make 会退回用主机的 gcc。
- 在仓库里任意位置调用即可，`wsl.exe` 会把当前的 Windows 目录映射进去。
- `OBJDIR` 指到仓库外面，产物不进版本库。默认值是程序目录下的 `build/`，已在 `.gitignore` 中。
- 升级 SoC IP、同步新 BSP 之后，改用 IDE 自带的工具链，这一段要跟着改。

## 上板运行

两种方式：

- 调试器加载：OpenOCD 连上后 `reset halt`，`load_image <bin> 0x1000 bin`，`resume 0x1000`。**加载前必须复位**，否则 CPU 取到的是指令缓存里的旧内容。**OpenOCD 要在 `sdk/bsp` 下启动**，在本目录的程序目录里启动会找不到 `cpu0.yaml`。
- 从 Flash 启动：把 bin 转成 hex（`tools/bin2hex.py`）写入 Flash `0x380000`，复位后由 bootloader 拷进 DDR。

两种方式的命令见 `../doc/开发环境.md` 第 6、8 节。

## bootSizeTest 的期望值

gcc 8.3.0 编译结果：bin 301,882 字节，数据块起于 `0x00001750`、长 300,000 字节，`crc_all = 0x47df83a8`，`crc_tail = 0x78c8c48f`。程序或工具链变化后要重新计算，构造与对照组设计见 `../../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md` 3.5 节。

## ddrAliasTest 的期望值

gcc 8.3.0 编译，bin 2418 字节。串口逐行打印 `LOW k=0..27`、`LOW k=-1`、`HIGH k=28..31`、`CACHE_NOEVICT`、`CACHE_EVICT`，全部为 `ok`，最后一行之前是 `base 0x0c800000 fail 0`。其中 `CACHE_NOEVICT` 读到 `11`（缓存里的旧值）是预期现象，不计入 `fail`。实验设计与原始输出见 `../../Engineering/TechReports/platform/02-DDR地址译码与AXI-ID实测.md`。

## ddrBurstProbe 的期望值

需要带 `axi_probe_master` 的比特流（2026-09-17 起）。gcc 8.3.0 编译，bin 3922 字节，`resume` 后 1 秒内输出完毕（2026-09-17 实测，全部 56 行在 0.5 秒左右内到达）。串口末尾为 `fail 0` 与 `done`，`IDENT`、`CROSS`、`SWEEP` 每行为 `ok`，`SHARE errs=0x00000000 ok`。周期数随板子会有约 2% 的波动，参考值与解释见 `../../Engineering/TechReports/platform/03-DDR3上的第二个主机与突发预算.md` 5.4 节。探针主机超时或挂死时 CPU 也访问不了 DDR，串口会停在最后一行，重新加载比特流恢复。

## cacheCoherencyTest 的期望值

需要带 `axi_probe_master` 的比特流。gcc 8.3.0，默认编译档 `-Og`（`DEBUG=yes` 加 BSP 的 `DEBUG_OG?=yes`）。**加载前必须 `reset halt`**（`tools/run_openocd.sh` 已包含）；不复位时探针挂住的坏状态会让程序一行都不跑，现象与程序有 bug 相同。

| 产物 | 编译 | bin 字节 | SHA256 前 16 位 | 末尾 | 日志 |
| --- | --- | ---: | --- | --- | --- |
| 正常版 | `OBJDIR=build` | 4850 | `a4c88d7b4f3fd078` | `fail 0` | `logs/normal_1.log` ~ `normal_3.log` |
| 注错版 | `OBJDIR=build_inject INJECT=1` | 4986 | `f3170f2da4a4c4a9` | `fail 0`，四行 `FAIL (inject)` | `logs/inject.log` |
| 注错自检版 | `OBJDIR=build_inject_selftest INJECT=2` | 4994 | `98287c7a6126d411` | `fail 4`，四行 `NOT_RED (inject)` | `logs/inject_selftest.log` |
| 正常版 `-O0` | `OBJDIR=build_o0 DEBUG_OG=no` | 9098 | `dfb5b2d3f767e29d` | `fail 0` | `logs/o0_normal.log` |
| 注错版 `-O0` | `OBJDIR=build_o0_inject DEBUG_OG=no INJECT=1` | 9146 | `974d605510654c6f` | `fail 0`，四行 `FAIL (inject)` | `logs/o0_inject.log` |

正常版每行 `ok`，A 组跑两遍所以 A 的行各出现两次：

- `P cpu_to_prb`：CPU 经别名放进去的值被探针读回；
- `A/B/C ... armed=1`：装行读到的确实是旧值，这一条为 0 时同组的判据作废；
- `A immediate`、`A after_loads`：读到旧值 `c0de00a1`，即缓存遮蔽了外部写入；
- `A invalidated_after`、`B invalidate_all`、`C invalidate_addr`：失效之后读到探针写入的新值；
- `D evicted`、`D word1`：驱逐之后读到新值，这是判据自检。

注错版变红的恰好是 A3 两行加 B、C 四行，A1、A2、D、P 与正常版相同。注错自检版保留失效调用、按注错模式判定，四行没变红就计入 `fail`，末尾必须是 `fail 4`；它证明注错宏没生效的产物不会得到 `fail 0`。

`TIME` 三行是失效代价，1 tick = 10 ns = 1 个 CPU 周期。三次正常版的 1000 次总量：空循环 13090~13104，全清 148044~148164（单次减空循环约 135.0 个周期），单地址 18094（约 5.0），全清加一次读 217707~217748（约 204.6）。这里的循环体写栈上的 `sink`，所以「全清」里含栈那一行重新装入的约 69 个周期，失效指令本身的代价看 `cacheMechTest` 的 T11。`probe_err` 一行都不应出现，出现即为探针异常并已计入 `fail`。

原始输出与结论见 `../../Engineering/TechReports/platform/05-CPU数据缓存与第二个主机写入的一致性.md` 第五节。

## cacheMechTest 的期望值

需要带 `axi_probe_master` 的比特流。gcc 8.3.0，`-Og`，bin 6906 字节（`sha256:21dccb3fd0e8a20b…`）。每组一行数据加一行 `Tn ok`，末尾 `fail 0` 与 `done`（`logs/run_1.log`、`run_2.log`）。各组的期望：

- `T1 mismatch=0`；`T2 write_only=0 write_then_read=16 alias_then_read=16`；`T3 armed=16 stale_after_probe=16 probe_bad=0`；
- `T4 armed_mask=ffffffff evict_mask=fffff000 bad=0`；`T5 stale=8`；
- `T7`、`T8`、`T9` 每个数都是 8；
- `T10` 六个数依次是 `8 0 8 8 0 8`（跳过的失效指令紧跟分支时仍清缓存，隔 1 条或 2 条 `nop` 不清）；
- `T11` 只打印不判定，参考值：`empty=9016 all=76016 addr=13019 two_hits=10082`，`two_misses` 约 147900~147970；
- `T6 mcause=00000002` 两次（CSR `0x810` 不存在）。

启动行打印 `sp_row`（栈顶所在的缓存行号，当前是 43）。它若落进某组用的行号段，那一组可能被栈访问挤掉，要先换地址。

## cacheMinTest 的期望值

不需要探针主机。gcc 8.3.0，`-Og`，bin 2690 字节（`sha256:ea0556fddb1f214a…`，改注释前后不变）。串口逐行打印 `S1`、`S2`、`S3` 三组，末尾 `fail 0` 与 `done`（`logs/run_1.log`）：

- `S3 CACHE_NOEVICT` 按设计报 FAIL（读到 `22`），不计入 `fail`；`S3 CACHE_EVICT` 为 `ok`；
- `S1` 三项与 `S2` 三项都 `ok`。

**这个程序只作「现象可复现」保留，不作为判据。** `S3` 在别名写之前先读了 `Q ^ EVICT`，那一步本身就把 `Q` 的行换出去了，它的 FAIL 证明不了「写之后读一次不装行」。装行规则以 `cacheMechTest` 的 T2 为准：只写不装行，写之后读一次会装行。过程见 platform 第 05 章 4.3 节。
