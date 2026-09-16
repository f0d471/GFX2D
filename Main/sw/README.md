# sw：本项目编写的 RISC-V 程序

与 `sdk/` 的分工：`sdk/` 是 IP 生成的 BSP 和官方示例，重新生成 SoC IP 时会被覆盖；本目录放本项目编写的程序，不会被生成器碰到。

程序仍然沿用官方的 makefile 体系，每个程序的 makefile 里用 `STANDALONE` 指到 `sdk/software/standalone`，BSP 路径由 `bsp.mk` 从中推出。

## 目录

| 目录 | 说明 |
| --- | --- |
| `bootSizeTest/` | 检查 bootloader 从 Flash 拷贝的长度。带一个 300,000 字节、内容随位置变化的数据块，启动后打印整体与最后 4KB 的 CRC32 |
| `ddrAliasTest/` | 实验 E2：逐位翻转地址检查 DDR 地址译码与回绕，并演示数据缓存遮蔽别名的现象 |
| `ddrBurstProbe/` | 实验 E4：经 APB 驱动 `axi_probe_master`，做 CPU 与第二个主机的交叉读、突发长度扫描、并发实验 |

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
