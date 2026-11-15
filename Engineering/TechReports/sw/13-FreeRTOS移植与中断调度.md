# 13 FreeRTOS移植与中断调度

2026-10-26～29的移植与修复记录。对象为FreeRTOS RISC-V端口、CLINT tick、PLIC中断桥和调度等待；
应用状态机见 [应用生命周期](15-应用生命周期与故障恢复.md)，公共API见 [平台SDK](16-平台SDK与资源所有权.md)。
不改RTL或生成的SoC SDK，不把桌面模型作为真实IRQ或最坏中断时延证据。

## 1 内核与板级集成

采用Efinity RISC-V IDE同源的FreeRTOS V202212.01，内核位于`Main/sdk/software/freeRTOS/kernel/`，
本板适配位于`Main/sw/rtos/`。RV32IM无A/C使用RISCV_MTIME_CLINT_no_extensions端口。
`USE_RTOS=1`选择内核与适配，未设置时既有裸机入口保持原构建路径。
目标链接脚本预留64KiB主栈及`__freertos_irq_stack_top`；应用只使用公开接口。

FreeRTOS该版portASM.S的非timer异步中断进入弱符号
`freertos_risc_v_application_interrupt_handler`，不是旧demo的`portasmHANDLE_INTERRUPT`宏。
适配以强符号派发PLIC。第一次沿用旧宏时，板上PC停在默认死循环，改为实际端口入口后恢复。

trap已安装而调度器尚未启动的窗口仍可能接到IRQ；桥先检查`xTaskGetSchedulerState()`，
未启动时只派发硬件中断，不调用内核give/tick，避免污染任务链表。

## 2 独立tick与显示通知

早期覆盖初始化读`0x0f8bbff8`，正确mtime地址由CLINT基址与TIME offset得到`0xf8b0bff8`。
错误地址加上cmp读回0被误诊为硬件不支持timer，曾临时以60Hz场中断推进tick；该结论已推翻。

正确地址下的独立诊断：100Hz档两秒199次中断，1kHz档两秒1998次，异常均0，cmp读回仍为0。
因此删除错误覆盖，使用标准端口与BSP同源地址提供1kHz tick，显示ISR只派发帧通知。
不能以cmp是否能读回替代中断频率与异常计数判据。

## 3 等待、优先级与单写者

初版runner在present忙等，低优先级monitor一直不运行；把monitor升到inputd同级只能掩盖问题。
最终使用独立tick、可调度等待钩子和runner让出；历史产品配置runner优先级2、inputd优先级3，
优先级1的monitor仍持续推进。输入采样与解码的临界区边界见 [UART与状态协议](21-UART输入与状态协议.md)。

logq原为单写者，monitor并发写曾造成约5KiB积压与tick行丢失；monitor改为发布暂存状态，
由runner的log_pump转投。显示完成和引擎完成分别唤醒等待者，不用清软件标志冒充硬件完成。

## 4 历史验证与边界

2026-10-28修复后的产品记录为菜单fps_milli=60000、tick_ms每秒增长1000、irqA每秒约60，
idle和低优先级monitor均增长。该组持续时钟验证取代早期“tick与场中断一一对应”的临时方案。
进入失败和在途帧失败后恢复菜单，由应用管理器的独立故障判据验证，不计作正常零错误输出。

未设置USE_RTOS的旧程序曾分别用前后make规则全量重编，产物逐字节相同；host/lib正常测试通过。
最终软件与板上验收为2026-10-28～29的历史范围，冻结硬件B7不等于当前RTL重新综合。
本章不声称硬件watchdog、Flash启动、最长关中断时间或人工实体输入已被验证。
