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

每档随附 **同一个** `console-O2-perf.bin`（当前源码构建的游戏机前端，CAPS 自适应：
运行期读 CAPS 决定走哪条后端、显示哪些入口）。程序可由 manifest 的 source_commit
确定性重建，BIN 入档是为了「烧写即用」的自足性；`bin_sha256` 登记。

| 档 | 上板实测现象（2026-10-29，console-board） |
| --- | --- |
| B0 | 五应用全跑：菜单/about/press/demo 60fps，**stg 弹幕 2D 走直通硬件后端 60fps**；r4 三维无完成帧（无 RAST，定义内） |
| B3 | 菜单/demo/press/about/stg（条带后端）60fps 全跑；r4 三维缺席（无 RAST） |
| B5 | 菜单/demo/press/about/stg（PB 后端）60fps 全跑；r4 三维缺席（无 RAST） |
| B7 | 五应用全跑含 r4 三维；另经 r4-board 三场景 CRC/SW-HW 等价/注错/节拍 PASS |

菜单 CRC 随档位不同（B7 `24eeb9cf`、B3 `0ae7ecb9`/`cd59c789`）= 菜单按 CAPS 自适应
显示入口的正确表现。各档验证 run 见各 manifest 的 `bin_verified_by`。

## 替换门槛

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
