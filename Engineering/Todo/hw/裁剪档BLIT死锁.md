# 裁剪档 BLIT 死锁

状态：**进行中**。主工程默认构型切到 B0 交付基本档的尝试因本缺陷回滚到 B5；
BAND_HW 出向网悬空已修（FILL 全帧由此从挂死恢复为 frame0 金标准 PASS），
BLIT 源读流的收尾死锁未修。修复后切换动作见本文件开发日志，切换本身不再单独立条。

## 1 要解决什么

引擎可选开关工作点（engine 第 15/16 章）只做了静态判据（elaboration、厂商 map、
netlist 结构检查、寄存器契约），整帧功能从未在任何裁剪档上跑过——这正是当时
登记的欠账「逐档功能板测」的仿真面。把主工程切到 B0 后第一次跑 frame/ctrl
整帧回归，暴露两个缺陷：

1. `engine_top` 的 `GFX_HAS_BAND_HW` 出向网（`band_enable`、`band_start` 等十条）
   只有 ifdef 分支没有 else——关闭条带硬件时悬空成 z，经 geom 的
   `if(band_enable)` 传播成 x，几何窗口烂掉、整帧挂死。综合器把 x 优化掉，
   所以静态判据全绿。**已修**：补 else 常量 0 分支。
2. **BLIT 源读流死锁（未修）**：FILL 全帧正常（frame0 PASS），BLIT 执行若干笔
   源读后 mover 读流门控不再放行，geom 描述符悬着，命令表超时。全功能档 B5
   不受影响（frame/ctrl 历来 16/16、4/4）。

## 2 怎么切分

1. 复现：主工程 CANONICAL=B0，`regress_suites.sh` 全套；frame 任一含 BLIT 的
   用例（frame1..8）、ctrl 端到端向量 #1 起超时。
2. 定位：frame 台超时分支的 PROBE 探针（已留在 tb_engine_frame.v）。
   已知事实——geom 描述符本身正确（rd_addr=0x200100、bytes=0x100、beats=16、
   ARLEN=15）；卡点在 mover：`rd_busy=1, rd_status_seen=1, rd_stream_done=0,
   rd_open=0`，`rs_last_fire` 不再发生；cmd 已到 S_TERM（说明命令字节消费完毕）；
   geom 的 `rd_cidx` 已回绕（源读实际发生了多笔，像素已写 128~6464 个）。
3. 嫌疑：mover 读流门控 `rd_gate_done = rd_status_seen && rd_stream_done`
   （engine_mover.v 311 行）。`rd_stream_done` 只由 `rs_last_fire =
   rs_fire && core_rs_tlast`（377 行）置位；`core_rs_tready` 在命令取数期间
   接 `fs_tready`（364 行）。待查：多笔描述符序列中某笔的 tlast 与 rs_fire
   错拍、或 `rd_drain_help` / `rd_ctx_bg` 分支下 fire 计数的丢失。
   300MB 参考波形：`Main/sim/engine/frame/build/f1diag.vcd`（frame1，+VCD 生成）。
4. 修复后验证：frame 全套 16/16（B0 头）、ctrl 4/4 与注错、全套 regress_suites、
   七代表档中受影响档（B0/B1/B2，BAND_HW 关）重综合对照、B5 预处理 token 序列
   不变（else 分支不进全功能编译输入）。

## 3 判据

- B0 头下 frame 全部 16 checks PASS（含 11 帧金标准与 sprites 性能）；
  ctrl 159 向量 + 定向 PASS。
- 修复不触碰全功能分支：B5 头下 frame/ctrl 结果与修复前一致；
  `git diff` 确认 ifdef 开启分支之外无改动。
- 修复后按切换日志完成：CANONICAL→B0、`--sync` + `regs_gen`、双 `--check`、
  主工程整机四步（B0 读数 36386 XLR / 105 RAM / 16 DSP，core_clk +0.882/+0.026 ns
  已登记）、游戏生产 BIN 板测（build_board_play，HW/DDR 后端，帧 CRC 与 60 fps）。
- 板测前置：FT4232H USB 需在位（2026-10-16 板卡 USB 掉线，插拔恢复后执行）。

## 4 开发日志

2026-10-16：B0 切换尝试与回滚。
切默认（CANONICAL 上收 feature_config、regs_gen 无参默认跟随、双 check、
四项 feature 检查、主工程 B0 四步 36386/105/16 全过）之后，frame 首次在裁剪档
跑整帧：FILL（frame0）先因出向网悬空挂死，补 else 修复后 PASS；BLIT 全部
超时，ctrl 端到端同样超时。定位到 mover 读流门控收尾缺失后，超出本轮时间盒，
主工程回滚 B5（生成物已 --sync 回 0x346F），留本条接续。
同轮顺带落地：frame 台 CAPS 断言改按 `EC_CAPS_VALUE`（随构型）、超时探针、
`+VCD` 波形开关；regress_suites.sh 对机制专项套件（tex/band）按主工程宏记
NOT_APPLICABLE；`synth_presets.sh` 不再 unset PYTHONHOME、`check_g2_band.py`
补 `-I rtl/engine`（engine 第 16 章第七节的验收修复）。
