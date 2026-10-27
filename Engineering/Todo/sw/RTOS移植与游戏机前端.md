# SW10：FreeRTOS 移植与游戏机式前端

## 状态

**待人工检阅（2026-10-28 判据全链 PASS）**。收尾中发现并修复六个缺陷
（构建 stamp 清理漏洞、PERF 档、r4 键源、HOME 键位契约、logq 双写者、
monitor 饿死），过程与证据见 TechReport sw13 §4；阻塞产品线第二迭代
「RTOS + 摄像头周」。

## 1 要解决什么

现在的软件是「单 app 裸机」：`gfx2dGame` 一个 main 从上电跑到断电，模式切换
（基础 Demo / 弹幕 / 压力 / 三维）在应用内部用和弦完成，PC 侧靠控制面板发数字键。
没有操作系统，慢外设（摄像头、传感器）将来只能挤进游戏主循环，产品形态
（游戏机：开机进菜单、选内容、玩、退回菜单）也没有承载结构。

本条目引入 FreeRTOS 并建立游戏机式软件层：

1. **FreeRTOS 内核在板上跑起来**：内核源码入库（来源 Efinity RISC-V IDE
   2026.1 的 FreeRTOSv202212.01，与 `sdk/software/freeRTOS/` 的移植胶水同源），
   tick 走 CLINT mtime（RISCV_MTIME_CLINT_no_extensions 芯片头），外部中断经
   `external_interrupt_handler` 桥接到项目自己的 PLIC 派发（与 `board_irq.c`
   同一套 A=场起始/B=引擎完成纪律，另加帧信号量）。
2. **软件层重构**：新增 `sw/rtos/` 集成层（内核 + 中断桥，地位同 bsp，可见 sdk
   系统头）；新增 `apps/common/` 共享运行时（内存窗口、素材包、stg 表面槽、
   TEX 热集、后端覆盖层——从 `gfx2dGame/main.c` 的启动序列抽出成显式上下文），
   console 与 gfx2dGame 今后共用；`apps/console/` 游戏机前端 app。
3. **游戏机式前端**：应用框架（注册表 + enter/frame/leave 生命周期 + 输入事件）
   + shell 菜单（标题画面、纵向列表、光标、页脚提示，素材复用 pak 里的
   TITLE/FONT/DIGITS/ICONS）+ 内置应用 v1：弹幕游戏（stg_modes 复用）、
   三维演示（r4 复用）、系统信息。HOME = 三模式键（MODE1|2|3）同按 8 帧
   回菜单，复用 stg_chord 计帧器；不与游戏内和弦（上下/左右）和 EX 四向同按
   入口冲突。
4. **gfx2dGame 与既有判据零影响**：`bsp/`、`lib/`、`gfx2dGame` 源码不动；
   `mk/target.mk` 只加 `USE_RTOS=1` 守卫块，缺省路径逐字节不变。

## 2 怎么切分

| 工作点 | 产物 | 依赖 |
| --- | --- | --- |
| A 内核入库与构建接入 | `sdk/software/freeRTOS/kernel/`（内核+RISC-V 端口+heap_4，README 登记来源与清单）；`mk/target.mk` 的 USE_RTOS 守卫块；console 的 `FreeRTOSConfig.h` | 无 |
| B rtos 集成层 | `sw/rtos/rtos_irq.c`（PLIC 初始化、external_interrupt_handler、帧/引擎计数与帧信号量）+ console 入口（任务表：runner/输入/idle 统计） | A |
| C 共享运行时抽取 | `apps/common/stg_runtime.[ch]`：boot_mem/boot_asset/表面槽/TEX 热集/后端覆盖层，显式 `stg_runtime` 上下文；console 用它，gfx2dGame 暂不迁（避免判据回归，迁移列为后续） | 无 |
| D 应用框架与 shell | `apps/console/src/appfw.[ch]`（注册表/生命周期/HOME 和弦）、`shell.[ch]`（菜单模型）、`shell_ui.c`（绘制）、`app_game.c`/`app_r4.c`/`app_about.c` | B、C |
| E 板上判据与回归 | `tools/check/check_console_board.py`（input-script 驱动：菜单→游戏→HOME→三维→HOME 的行序断言）；gfx2dGame host quick 回归 | D |

## 3 判据

1. **构建**：`console-O2.bin` 构建成功且项目代码零告警（内核上游源码按 sdk 同类
   豁免，不适用 -Werror）；链接断言（≤512K 镜像、64K 主栈）通过；`USE_RTOS`
   不设置时 `mk/target.mk` 产物与改动前逐字节一致（gfx2dGame 双目标重编对比）。
2. **RTOS 存活**：板上 UART 出现 RTOS 横幅与周期 idle 统计行（tick 计数在走、
   任务数正确）；内核 tick 走 CLINT mtime，`board_cycles()` 语义不变。
3. **前端全链（板上，board_run.py --bin + --input-script）**：启动→菜单行与
   帧 CRC→进弹幕游戏（mode 2 enter 行）→HOME 三键和弦→回菜单→进三维
   （R4STAT/enter 行）→HOME 回菜单，全序列 UART 行序断言 PASS。
4. **游戏在 console 内不退化**：弹幕游戏在 console 内跑 SW 后端时板上一秒窗口
   swap 计数与场计数差为 0（不丢帧的既有口径，观察 ≥10 秒）。
5. **回归**：`gfx2dGame` host 侧 quick 判据（validate.py 受影响项：应用构建与
   host 运行）不红；`bsp/`、`lib/`、`gfx2dGame/src/` 源文件零改动。

## 4 开发日志

- 2026-10-28：板上判据全链 PASS（`check_console_board.py` 四项全过：行序
  10/10、menu fps 46~61、`rtos tick` 行出现且 tick==irqA、无错误行；原始日志
  `Main/sw/apps/console/logs/board_acceptance_20261028.log`）。收尾过程挖出
  六个缺陷并全部修复：①**mk stamp 清理枚举不净**（APP_SRCS 带 `../` 的应用
  对象落在 build/game、build/common，换档后旧对象混链——console 打开 PERF 后
  stg_modes.o 仍是旧档，每帧多 1.2MB CRC、4fps；OpenOCD PC 采样+addr2line
  定位；mk 改整删 OBJDIR，host.mk 同）②console 缺 PERF 档（游戏落回 SW
  2.5fps；makefile 改 PERF:=1，产物 -perf 后缀）③r4 条带循环直读 board_keys
  抢 UART 字节（加 r4_set_key_source 注入，裸机零变化）④app_r4 的 MODE3
  热键不避让 HOME 组合（和弦期每帧触发场景重进 ≈250ms；HOME 进行中热键
  让位）⑤logq 双写者（monitor 并发 put 打坏 head/len，队列积压 5KB 永不
  排空、tick 行全丢；monitor 改暂存发布、runner 转投）⑥monitor 优先级饿死
  （runner 忙轮询不阻塞，≤3 全饿死；monitor 提到与 inputd 同级 4）。
  判据脚本自身修正两处：HOME 按住期每 250ms 重发（board_keys 300ms 无包
  清零）、--input-script-after 对齐菜单横幅。回归：USE_RTOS=0 双编逐字节同
  （dd162cc8）、console 222KB/64K 栈断言、gfx2dGame 双目标零告警、lib 单测
  fail 0、板上回归 HW 60fps drop=0 / SW 2fps 与归档一致。遗留：sw6 quick 的
  D3 与 21 项包失配（10-25 批次未再生期望日志，非本轮引入）列入待办。
- 2026-10-26（晚）：A~E 工作点代码全部落地（内核入库 40 文件+MANIFEST、
  target.mk USE_RTOS 块、sw/rtos 桥、stg_runtime、console 全部源码、
  check_console_board.py、sw13 章 TechReport、sw/README 登记）。
  板上联调三次硬发现（详见 sw13 §3）：端口 .weak 派发点覆盖、CLINT mtimecmp
  异常→tick 改场驱动、调度器启动窗口门控；均已修复。
  已在板上验证：中断桥帧信号量、pak 对账、TEX 热集（B7 通过/旧位流正确报错）、
  menu crc `badb9e1a` 复现。**未完成**：引擎渲染菜单帧率判据、input-script
  全链、rtos tick 行——FT4232 USB 掉线（JTAG+串口同时消失，pnputil 无效，
  需重插 USB）打断，判据脚本待跑。回归：gfx2dGame 双目标构建零告警过，
  板上行为回归待 USB 恢复补跑。
- 2026-10-26 立项。前序调研结论（会话内静态分析）：内核取自 Efinity RISC-V
  IDE 2026.1 安装目录的 `rtos/FreeRTOS/FreeRTOSv202212.01`（本机展开位置见
  `Main/doc/开发环境.md` 第 11 节同类记录）；
  `sdk/software/freeRTOS/` 已有 IDE 出品的移植胶水与 `freertos.ld`（其
  `__freertos_irq_stack_top` 已同步进 `bsp/ti60/linker/gfx2d.ld`）；
  `board_irq.c` 的 trap()/PLIC 派发语义即 rtos 桥的蓝本；`board.c` 的
  present/wait 全部寄存器轮询 + ISR 计数，不依赖 mtvec 归属，rtos 侧
  不调 `board_irq_init` 即可与其并存（board_irq_enabled()==0 走寄存器轮询
  分支）。选型依据见 10-26 会话结论：FreeRTOS（厂商已移植）优先于 Zephyr
  （官方 fork 仅 UART/GPIO 驱动）。
