# snapshots：交付档位流快照

四档交付位流与其身份清单，**入库版本库**（约 2.5MB）。与 `outflow/`（最后一次构建，
可能是实验构型）和 `board_release_*/`（完整上板复现包，不入库）语义严格区分。

## 档位

| 目录 | 构型 | 综合模式 | 用途 |
| --- | --- | --- | --- |
| `B0/` | 基本档（像素全开） | speed | 默认交付档，CANONICAL |
| `B3/` | +TEX/BAND/STRIP/INST | speed | 条带基座 |
| `B5/` | +PB 全功能 | speed2 | 全功能档 |
| `B7/` | +RAST/RAST_TEX 竞技 | speed2 | 生产默认（游戏随 B7 上板） |

B1/B2/B4/B6 是消融测量档，按需构建不留快照；快照集与 `check_feature_synthesis.py`
的代表档矩阵共用构型定义（单一真源 `tools/check/feature_config.py`）。

## 默认 BIN 与各档预期现象

每档随附 `console-O2-perf.bin`、`content.json` 和 `firmware.json`。B0/B3/B5 使用同一份
`2d` 内容固件（demo/stg/press 加系统菜单/about），B7 使用 `full`（再加入 R4 与滑翔）。2D BIN
实际裁掉 R4、atlas 和专用库；运行时仍按真实能力保护所有入口，菜单只列可用内容。
本轮软件替换的验证状态与原始运行引用见各 manifest 的 `bin_verified_by`。
硬件 `source_commit` 不代表新软件版本；软件编译器、参数、预算、程序/素材摘要与源码
状态独立登记在 firmware.json，content.json 绑定实际编入的应用集合。

B7、B5 另附 `assets.pak`（manifest 的 `assets` 记摘要与 CRC32）：BIN 里写死了素材包 CRC，
两者必须成对写进 Flash（程序 `0x380000`、素材 `0x400000`），位流分区不动。2026-11-02 起
B7 的 BIN/素材为 `bce326d` 版（Kenney 2D 素材、删茶杯、滑翔 SW20），与当时板上 Flash 逐字节相同；
B5 同日换上同一份代码编出的 `2d` 版 BIN（演示/弹幕/压力测试/系统信息），经 JTAG 实板核对，
Flash 仍是 B7。B0/B3 的 2D BIN 仍是 10-30 版，绑定当时的旧素材包（摘要见各自 firmware.json），
与仓库现在的素材包 CRC 不同，要上板须先重编。

| 档 | 配套内容及能力边界 |
| --- | --- |
| B0 | 2d；基础像素引擎，无 TEX/RAST |
| B3 | 2d；有 TEX/BAND/INST，无 RAST |
| B5 | 2d；增加 PB，无 RAST |
| B7 | full；有 RAST/RAST_TEX，无 PB |

菜单 CRC 与内容集合/选择有关，不能把旧菜单 CRC 当新布局的判据。旧单 BIN 的
2026-10-29 验证保留在历史记录；当前软件以目录、真实出帧和绑定的位流/BIN身份验证。
PB 与 RAST 互斥，不能按 B0/B3/B5/B7 编号高低推断兼容性。

## 替换门槛

本节三项用于替换位流。仅更新软件时保留原 bit 摘要，按
[`../../doc/验证流程.md`](../../doc/验证流程.md) 完成所选软件构建与对应档的实板检查，
分别更新软件身份；不因换 BIN 重做无改动的综合。

快照跟随源码批次更新（如 PPA 优化批），任一档的编译输入变化后：

1. 该档四步综合（map/interface/pnr/pgm）PASS；
2. `feature-contract`、`feature-function` 宿主判据 PASS（关闭态契约与 220 构型矩阵）；
3. **拟部署到板的档**加板测（board smoke 或对应 board 判据），其余档默认不做板测。

三项齐了才替换对应档的 `gfx2d_ti60.bit` 并重新生成 `manifest.json`。综合器的源码文本
敏感性（同源码波动 2~3%）意味着「看起来没变的档」也可能漂移：只重排不改行为的批次
（如 engine_pix 2026-10-26 重排）同样触发受影响档的重综合与替换。

## manifest.json

由 `tools/board/bit_manifest.py` 生成，是快照身份的唯一真源：构型名、CAPS、综合模式、
源 commit、XLR/RAM/DSP、core_clk setup/hold、位流 SHA256、验证 run 引用与日期。
软件兼容性以 manifest 的 CAPS 为准——**不是每个游戏能跑在每个档上**：无 RAST 的档
（B0~B5）不能跑三维场景，运行期按 CAPS 探明并拒绝/降级（BSP 已实现），启动器按
manifest 过滤入口。

## 回滚

git 历史即归档：`git log --oneline -- Main/bit/snapshots/B7/` 找旧版，
`git checkout <commit> -- Main/bit/snapshots/B7/` 恢复。

## 已知坑

- `tools/board/board_run.py --flash-boot` 默认烧 outflow 位流=B0 形态，会把 Flash
  `0x0` 覆写成非生产位流；生产部署走 `make_flash_image.py` 合 B7 整镜像
  （详见 bit/README 与 TechReport sw/12 章）。
