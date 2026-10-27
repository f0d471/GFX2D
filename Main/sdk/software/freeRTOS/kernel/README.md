# kernel：FreeRTOS 内核源（本项目补入）

来源：Efinity RISC-V 嵌入式 IDE 2026.1 安装目录
`rtos/FreeRTOS/FreeRTOSv202212.01/FreeRTOS/Source`（FreeRTOS Kernel V202212.01，
MIT，见 `LICENSE.md`）。这是 IDE 给 Sapphire SoC 生成 FreeRTOS 工程时引用的
同一份内核——`../freertosDemo/makefile` 里的 `${FreeRTOS_KERNEL_SRC}` 指向它。

为什么拷进来而不是引用安装目录：仓库规则不写绝对路径，且 IDE 安装路径不属于
仓库的可复现输入。逐字节清单见 `MANIFEST.sha256`
（`sha256sum -c MANIFEST.sha256` 可复核；`Documentation.url` 未拷贝）。

拷贝范围（其余上游目录未入库）：

| 内容 | 路径 |
| --- | --- |
| 内核 7 个 .c（tasks/queue/list/timers/event_groups/stream_buffer/croutine） | `*.c` |
| 内核头文件 | `include/` |
| RISC-V GCC 端口（port.c、portASM.S、portmacro.h、portContext.h） | `portable/GCC/RISC-V/` |
| 芯片专用扩展头（本 SoC 用 `RISCV_MTIME_CLINT_no_extensions`：标准 CLINT mtime，无附加上下文） | `portable/GCC/RISC-V/chip_specific_extensions/` |
| 内存管理 heap_4 | `portable/MemMang/heap_4.c` |

构建入口在 `Main/sw/mk/target.mk` 的 `USE_RTOS` 块（SW10），消费方是
`Main/sw/apps/console/`。上游源码按 sdk 同类豁免不适用项目的 -Werror 纪律，
编译选项与普通对象分开写在该块里。
