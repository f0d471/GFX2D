# tools：辅助工具

按用途分五类，每类一个目录，各自的 README 写用法与注意事项。

| 目录 | 用途 | 工具 |
| --- | --- | --- |
| [`board/`](board/README.md) | 上板：烧比特流、加载程序、收串口、读写寄存器、写 Flash 前转格式与合成镜像 | `board_run.py`、`gfx2d_debug.cfg`、`bin2hex.py`、`make_flash_image.py` |
| [`soc/`](soc/README.md) | 重新生成 Sapphire SoC IP 与软件包 | `ipm_gen_soc.py` |
| [`assets/`](assets/README.md) | 素材：占位 PNG 生成、游戏的霓虹几何素材生成、按 manifest 打成素材包并生成 `assets.h`、上游点阵字体转成字库文本 | `gen_placeholder.py`、`gen_stg_neon.py`、`gen_font_txt.py`、`pack_assets.py` |
| [`check/`](check/README.md) | 提交前检查与规格校验：路径占位符、软件分层判据、像素舍入穷举、混合方程代入表、命令语义一致性向量、**引擎几何层的独立模型与整帧用例导出**、素材包、基础 Demo 的录像与日志、游戏本体的数值基础、实体池与弹型、通关录像的搜索、统计埋点的基础件与出图 | `localize_paths.py`、`check_sw_layers.sh`、`check_round255.c`、`check_subst.c`、`check_cmd_semantics.py`、`geom_model.py`、`geom_cases_dump.c`、`geom_cases_dump.sh`、`check_sw5_assets.sh`、`check_sw5_pak.py`、`check_sw6_demo.sh`、`check_sw6_log.py`、`gen_sw6_replay.py`、`check_sw7_game.sh`、`gen_sin_tab.py`、`gen_stg_patterns.py`、`gen_stg_clear.c`、`check_sw8_stats.sh`、`check_sw8_tx.c` |
| [`stats/`](stats/README.md) | 统计日志出图：把模式框架的 CSV 与 `N_max` 搜索日志画成报告与幻灯片用的图的源码 | `plot_stats.py` |

## 用哪个解释器

| 工具 | 在哪跑 | 为什么 |
| --- | --- | --- |
| `board/board_run.py` | Windows，Efinity 自带的 python，设 `PYTHONHOME=<EFINITY>\python311` | 要 pyserial 与 Windows 的 COM 口；WSL 拿不到 COM 口 |
| `soc/ipm_gen_soc.py` | Windows，`call <EFINITY>\bin\setup.bat` 之后的 `python` | 要 IP Manager 的库与 `devicedb` 的 DLL |
| `board/bin2hex.py`、`check/localize_paths.py`、`check/check_sw5_pak.py` | 任意 python3，本机用 `wsl -e python3` | 只用标准库 |
| `assets/gen_placeholder.py`、`assets/gen_stg_neon.py`、`assets/pack_assets.py` | 同上，但要 Pillow（2026-09-22 复测：python 3.14.4、Pillow 12.2.0） | 要解 PNG；只用标准库 + Pillow，不依赖 numpy |
| `board/make_flash_image.py` | 同上，只用标准库 | 合成与回环校验，不碰板子 |
| `check/check_cmd_semantics.py`、`check/geom_model.py` | 同上，只用标准库 | 生成与校验命令语义的一致性向量；引擎几何层的独立模型（求交、描述符、边带、定向用例、$readmemh 记录导出），不读 RTL、不依赖板子与编译器 |
| `check/geom_cases_dump.sh` | WSL，要主机 gcc | 链接 `sw/lib/gfx` 的 `cases.c` 与 `list.c`，把十一个整帧用例导成几何模型能吃的记录 |
| `check/check_sw6_log.py`、`check/gen_sw6_replay.py` | 同上，只用标准库 | 判据日志的外部核对（`zlib.crc32` 与 `lib/core/crc32` 同参）；SW6 两段录像的独立生成 |
| `check/gen_sin_tab.py` | 同上，只用标准库 | 生成 SW7 的 1280 项 Q1.14 正弦表 `sw/lib/core/sin_tab.c` |
| `check/gen_stg_patterns.py` | 同上，只用标准库 | SW7 弹型判据的独立实现：生成五种弹型的期望文件，并与 C 的轨迹逐帧比对 |
| `stats/plot_stats.py` | 同上，只用标准库 | 读统计日志、出 HTML+SVG 的图源；导出 PNG 用 `Deliverables/Assert/figures/src/render.sh`（Windows 的 Edge） |
| `check/check_sw_layers.sh`、`check/check_sw5_assets.sh`、`check/check_sw6_demo.sh`、`check/check_sw7_game.sh`、`check/check_sw8_stats.sh`、`check/check_sw8_tx.c`、`check/gen_stg_clear.c`、`check/check_round255.c`、`check/check_subst.c`、`check/geom_cases_dump.c` | WSL | 要交叉工具链、主机 gcc；前两个与 `check_sw6_demo.sh`、`check_sw7_game.sh`、`check_sw8_stats.sh` 都要 `python3`（`check_sw7_game.sh` 打包素材还要 Pillow）；`check_sw7_game.sh`、`check_sw8_stats.sh` 要 `gcc-multilib` 与 `libsdl2-dev`，`check_sw8_tx.c` 要 `libsdl2-dev`（由 `check_sw8_stats.sh` 编译）；`geom_cases_dump.c` 只要主机 gcc，由 `geom_cases_dump.sh` 编译 |

Efinity 自带的 `python.exe` 在 `python311\bin\` 下而标准库在 `python311\Lib`，Python 推不出 prefix，
不设 `PYTHONHOME` 时报 `ModuleNotFoundError: No module named 'encodings'`（不是缺标准库）。
`setup.bat` 会设好它，所以 `ipm_gen_soc.py` 那条不用另设。

## 加新工具的规则

- 放进上面五类之一；都不合适时再开新目录，并在本表登记。
- 只做一次的实验脚本、临时诊断程序不进 `tools/`。实验程序放 `sw/apps/probe/`，
  一次性的核对做完就删，结论与过程写进 `Engineering/TechReports/`。
- 不写绝对路径：工具位置按环境变量或相对本脚本的位置推，仓库根由脚本位置推出，不要求从特定目录运行。
- 判断成功要有正面证据（输出里的成功标记、字节数、`done`），不能只看子进程的退出码：
  厂商脚本出错时也可能返回 0，见 `board/README.md`。
- 判据脚本要带注错：把一处改错，判据必须报 FAIL，只跑正常用例说明不了判据在检查东西。
