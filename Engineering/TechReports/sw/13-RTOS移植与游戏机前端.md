# 13. RTOS 移植与游戏机前端（SW10）

状态：**板上判据全链 PASS（2026-10-28），待人工检阅**。
条目：[`../../Todo/sw/RTOS移植与游戏机前端.md`](../../Todo/sw/RTOS移植与游戏机前端.md)。

## 1. 选型与依据

2026-10-26 会话静态分析 + 上游核实定案：**FreeRTOS**。依据：

- 仓库里 Efinity IDE 出品的移植胶水齐全（`sdk/software/freeRTOS/`：demo、
  `freertos_risc_v_chip_specific_extensions.h`、`freertos.ld`，且
  `bsp/ti60/linker/gfx2d.ld` 早已预埋 `__freertos_irq_stack_top`）；
- Zephyr 官方 fork（Efinix-Inc/zephyr-efinix）只有 UART/GPIO 驱动，I2C/SPI
  要自写；FreeRTOS 侧这些驱动头文件 sdk 全有；
- ISA（RV32IM 无 A/C）恰是 FreeRTOS 官方 `RISCV_MTIME_CLINT_no_extensions`
  芯片头覆盖的组合；Zephyr 主流组合是 rv32imac；
- 产品（游戏机）无连接栈需求，Zephyr 的增值用不上。

## 2. 软件层（重构式建立）

```
sdk/software/freeRTOS/kernel/   内核 vendored（FreeRTOSv202212.01，来源 Efinity
                                RISC-V IDE 安装，MANIFEST.sha256 逐文件可核）
sw/rtos/                        FreeRTOS 集成层：外部中断桥 + 帧信号量 + tick 接管
sw/apps/common/stg_runtime      共享启动运行时（素材包对账/stg+r4 表面槽/TEX 热集/
                                后端覆盖层，从 gfx2dGame/main.c 抽出并上下文化；
                                gfx2dGame 本体暂不迁移，判据零影响）
sw/apps/console/                游戏机前端 app：FreeRTOSConfig/appfw（注册表/
                                enter-frame-leave 生命周期/HOME 和弦）/shell 菜单/
                                三个内置应用适配器（stg、r4、about）
mk/target.mk                    USE_RTOS=1 守卫块（不设置时既有应用零变化）
```

任务模型：runner（prio3，64K 栈，逐帧驱动当前应用）/ inputd（prio4，每场采样
board_keys——独占调用者，合并 held|沿）/ monitor（prio4，10s 一行存活证据，
只在菜单时说话；**必须与 inputd 同级**，见 4.1 的饿死发现）。HOME = 三模式键
同按 8 帧（复用 stg_chord 的 armed 语义），不与游戏内和弦（上下/左右）和
EX 四向入口冲突。日志纪律同裸机：全部行进 logq，每帧按 board_tx_room() 泵出。

## 3. 板上实测的三条发现（本轮的硬结论）

1. **端口版本差异**：FreeRTOSv202212.01 的 GCC/RISC-V 端口 `portASM.S` 不引用
   `portasmHANDLE_INTERRUPT`（freertosDemo makefile 里那个 -D 是旧版残留）；
   非 timer 异步中断一律进 `.weak freertos_risc_v_application_interrupt_handler`
   （缺省死循环）。集成层以强符号覆盖它派发 PLIC——第一版按 demo 的 -D 写法
   板上静默挂死，OpenOCD 读 PC 停在该死循环定位。
2. **CLINT 定时器通路异常**：mtime 正常（board_cycles 的基准），mtimecmp
   「写生效、读恒 0」，且硬件 timer 中断实际 rate 与配置的 60Hz 不符
   （实测 ~1250/s，机制未明）。对策：覆盖 `vPortSetupTimerInterrupt`
   （port.c 的 weak）把 cmp 写到极远未来让硬件 timer 分支休眠，tick 的唯一
   来源改为场起始中断 A（每场 xTaskIncrementTick，模拟端口 tick 路径，
   调用约定与 portASM 的 timer 分支一致）。`configTICK_RATE_HZ=60`，
   vTaskDelay 的粒度是一场 16.7ms；产品要毫秒级 tick 须修 SoC IP 重综合。
3. **内核 API 的启动窗口**：main 装 trap 到 `vTaskStartScheduler` 之间若来
   中断，在调度器未起来时调内核 API（give/tick）会污染任务链表（现象：
   后续 sem 唤醒时好时坏、delay 永不醒）。桥里以 `xTaskGetSchedulerState()`
   门控：未启动只派发不碰内核。

另有一条工程事实：整屏 SW 光栅在 100MHz VexRiscv 上 ~3fps（与控制面板
「SW 帧率较低」的既有口径一致），菜单/关于页因此走引擎渲染
（renderer_prepare(1)，无引擎的老位流退 SW）。

## 4. 板上验证状态：判据全链 PASS（2026-10-28）

`check_console_board.py` 四项全过（原始日志
`sw/apps/console/logs/board_acceptance_20261028.log`）：

1. **行序**：console boot → pak → enter menu → menu crc（`badb9e1a` 复现）→
   enter stg → leave stg → enter menu → enter r4 → leave r4 → enter menu，
   10 个标记按序出现；
2. **引擎渲染菜单帧率**：menu fps 46~61（判据 ≥45）；
3. **RTOS 存活**：`rtos tick 1200 heap 57920 tasks 4 irqA 1200`——tick 与
   场中断严格同步（tick==irqA），堆稳定；
4. **无错误行**。

游戏不退化（console 内 stg 应用的每秒证据行，见
`board_first60fps_20261028.log`）：`stg fps 61 field 61 swap 61 backend 1`——
一秒窗口 swap 数等于场数（零丢帧既有口径），HW 后端 61fps。

### 4.1 判据收尾挖出的六个缺陷（全部修复，均为 SW10 引入或暴露）

1. **构建系统：stamp 清理枚举不净（危险级，超出 SW10 的通用缺陷）**。
   stamp 变更时按子目录枚举删 `$(OBJDIR)/app lib bsp …`，但 APP_SRCS 带
   `../` 上跳路径的应用（console 复用游戏源码、共享运行时）会把对象写到
   `build/game`、`build/common`——枚举删不干净，**换编译档后旧对象混进新链**。
   实测：console 打开 PERF 后 `stg_modes.o` 仍是旧档（无 SW9_PERF），每帧多
   做一次 1.2MB 整帧 CRC，游戏 4fps、HOME 和弦 8 帧永远凑不满。这正是
   sw/README「源码与产物脱钩」踩过的同类坑，按子目录枚举修不掉，改为整删
   OBJDIR（`mk/target.mk`、`mk/host.mk` 同处注释）。定位手段：OpenOCD 连续
   PC 采样 + `addr2line`（四发全中 crc32_update、RA 指向内联的整帧 CRC）。
2. **console 缺 PERF 档**：游戏帧的引擎流水由 SW9_PERF 门控
   （`renderer_pipeline_enabled`），console 构建没开 → 游戏落回软件执行器
   （板上 2.5fps，与归档 SW 档特征值吻合）。console makefile 改 `PERF := 1`
   （产物 `console-O2-perf.bin`，与裸机 -perf 档同语义）。
3. **r4 在框架外直读 board_keys**：r4_render 的条带循环每帧 30 次直读
   UART 键解析器，与 inputd 的独占约定冲突（裸机里它是唯一读者所以无害）。
   加 `r4_set_key_source()` 注入（缺省 NULL=直读，裸机零变化；console 注入
   `appfw_input_held()`）。
4. **app_r4 违反键位契约**：appfw.h 明文「MODE 键归框架，应用处理会与 HOME
   冲突」——r4 的 MODE3 热键不避让 HOME 组合，按住 HOME 的每帧都被当成
   场景切换、帧边界重进（TEX 重装+清屏+等场 ≈250ms/帧），8 帧窗口凑不满。
   修复：HOME 和弦进行中应用热键全部让位。
5. **logq 双写者**：monitor 并发 `logq_put` 与 runner 侧 telemetry/put 竞争
   ——logq 是单写者设计（head/len 非原子），板上实测队列积压 5KB 永不排空、
   rtos tick 行全部丢失。修复：monitor 改发布到暂存缓冲，log_pump 在 runner
   上下文转投（单写者约定恢复）。
6. **monitor 优先级饿死**：runner 整循环从不阻塞（present 等场是寄存器忙
   轮询，board.c 与裸机共用、不走帧信号量），任何优先级 ≤3 的任务永久饿死
   （60s 菜单驻留 monitor 零唤醒，s_mon_wakes 计数为证）。monitor 提到与
   inputd 同级（4）；runner 改为在 present 上阻塞帧信号量是后续改进项。

### 4.2 判据脚本自身的两处修正

- 输入脚本的按住必须每 250ms 重发：`board_keys()` 对串口键包有 300ms 无包
  自动清零（为真实手柄持续转发设计），一次性发一拍在板侧只等效按住 300ms；
- `--input-script-after` 起点对齐 `console enter menu`（素材装载 ~7s，按
  resume 起算的绝对时刻会落进装载期）。

## 5. 回归（2026-10-28）

- **USE_RTOS 不设置时 target.mk 零影响**：当前 mk 与 HEAD mk 各全量重编
  gfx2dGame，产物 SHA256 逐字节相同（`dd162cc8cbebdd21…`），链接断言
  （console 222,022 B < 512K 镜像、`__freertos_irq_stack_top` 64K 主栈）通过；
- gfx2dGame 双目标（target/host）构建零告警（r4_render/r4.h 改动后）；
- lib/host 单测 `fail 0`；
- sw6 quick 判据：D1/D2/D9×4/D10×3 全过；**D3 与入库期望日志失配是 10-25
  素材批次（21 项包）后未再生期望的遗留账**（期望日志停在 09-28 的 20 项包
  时代），非本轮引入，再生列入待办；
- **gfx2dGame 板上行为回归**（B7，`game_regression_hw_20261028.log`）：
  HW 后端 fps=60、drop=0、零故障；默认 SW 后端 2fps 与归档裸机软件档一致；
  mode 2 enter 的 clip/pool 与 PC 参考一致（PERF 档按设计跳逐帧 CRC）。

## 6. 待办（后续工作点）

- **sw6 期望日志再生**：全量+quick 两份按 21 项包重录入库（判据卫生，
  与素材批次 94caa7f 的欠账）；
- gfx2dGame 迁移到 stg_runtime 的启动序列去重（SW14）；
- runner 在 present 上阻塞帧信号量（消掉忙轮询，monitor 即可降回低优先级）；
- console 的 host 侧菜单逻辑单测（OSAL 化，SW14）；
- SoC IP 层修 CLINT/timer（毫秒级 tick）或启用 USER_TIMER0，产品线视需要；
- 摄像头应用（产品线第二迭代）叠在 appfw 上。

（2026-10-28 判据收尾前的旧 5/6 节并入上文 4.1/5/6：裸机 trap 的行为保持
抽取说明见 5 的第一条；原「随 USB 恢复补跑」的板上回归已完成，见 5。）

## 7. 证据日志

| 文件 | 是什么 |
| --- | --- |
| `sw/apps/console/logs/board_acceptance_20261028.log` | 判据全链 PASS 的原始串口日志 |
| `sw/apps/console/logs/board_first60fps_20261028.log` | 首次 60fps（PERF 档修复后）的 stg 每秒证据行 |
| `sw/apps/console/logs/game_regression_hw_20261028.log` | gfx2dGame 板上回归（HW 后端 60fps drop=0） |
| `sw/apps/console/logs/game_regression_sw_20261028.log` | 同上（默认 SW 后端 2fps，与归档一致） |
