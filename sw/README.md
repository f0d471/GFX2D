# sw：本项目编写的 RISC-V 程序

与 `sdk/` 的分工：`sdk/` 是 IP 生成的 BSP 和官方示例，重新生成 SoC IP 时会被覆盖；本目录放我们自己写的程序，不会被生成器碰到。

程序仍然沿用官方的 makefile 体系，每个程序的 makefile 里用 `STANDALONE` 指到 `sdk/software/standalone`，BSP 路径由 `bsp.mk` 从中推出。

## 目录

| 目录 | 说明 |
| --- | --- |
| `bootSizeTest/` | 检查 bootloader 从 Flash 拷贝的长度。带一个 300,000 字节、内容随位置变化的数据块，启动后打印整体与最后 4KB 的 CRC32 |

## 编译

本机的 RISC-V IDE 是 2026.1，自带 gcc 13.4，编不了 2025.2 生成的 BSP（缺 `_zicsr`）。当前用 WSL 里的 xPack gcc 8.3.0：

```
wsl -e bash -c 'X=~/opt/xpack-riscv-none-embed-gcc-8.3.0-2.3/bin/riscv-none-embed-; \
  cd /mnt/e/Workspace/Efinix/Main/sw/bootSizeTest && \
  make BSP=efinix/EfxSapphireSoc RISCV_BIN=$X OBJDIR=<输出目录>'
```

- 必须写 `wsl -e`，否则命令会先经过一层 shell，`$X` 被提前展开成空，make 会退回用 x86 的 gcc。
- `OBJDIR` 指到仓库外面，产物不进版本库。默认值是程序目录下的 `build/`，已在 `.gitignore` 中。
- 升级 SoC IP、同步新 BSP 之后，改用 IDE 自带的工具链，这一段要跟着改。

## 上板运行

两种方式：

- 调试器加载：OpenOCD 连上后 `reset halt`，`load_image <bin> 0x1000 bin`，`resume 0x1000`。**加载前必须复位**，否则 CPU 取到的是指令缓存里的旧内容。
- 从 Flash 启动：把 bin 转成 hex（`tools/bin2hex.py`）写入 Flash `0x380000`，复位后由 bootloader 拷进 DDR。

两种方式的命令见 `doc/板卡实测记录.md` 第 6、8 节。

## bootSizeTest 的期望值

gcc 8.3.0 编译结果：bin 301,882 字节，数据块起于 `0x00001750`、长 300,000 字节，`crc_all = 0x47df83a8`，`crc_tail = 0x78c8c48f`。程序或工具链变化后要重新计算，做法见 `doc/板卡实测记录.md` 7.4 节。
