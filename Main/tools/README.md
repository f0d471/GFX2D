# tools：辅助工具

按用途分三类，每类一个目录，各自的 README 写用法与注意事项。

| 目录 | 用途 | 工具 |
| --- | --- | --- |
| [`board/`](board/README.md) | 上板：烧比特流、加载程序、收串口、读写寄存器、写 Flash 前转格式 | `board_run.py`、`gfx2d_debug.cfg`、`bin2hex.py` |
| [`soc/`](soc/README.md) | 重新生成 Sapphire SoC IP 与软件包 | `ipm_gen_soc.py` |
| [`check/`](check/README.md) | 提交前检查与规格校验：路径占位符、软件分层判据、像素舍入穷举 | `localize_paths.py`、`check_sw_layers.sh`、`check_round255.c` |

## 用哪个解释器

| 工具 | 在哪跑 | 为什么 |
| --- | --- | --- |
| `board/board_run.py` | Windows，Efinity 自带的 python，设 `PYTHONHOME=<EFINITY>\python311` | 要 pyserial 与 Windows 的 COM 口；WSL 拿不到 COM 口 |
| `soc/ipm_gen_soc.py` | Windows，`call <EFINITY>\bin\setup.bat` 之后的 `python` | 要 IP Manager 的库与 `devicedb` 的 DLL |
| `board/bin2hex.py`、`check/localize_paths.py` | 任意 python3，本机用 `wsl -e python3` | 只用标准库 |
| `check/check_sw_layers.sh`、`check/check_round255.c` | WSL | 要交叉工具链、主机 gcc |

Efinity 自带的 `python.exe` 在 `python311\bin\` 下而标准库在 `python311\Lib`，Python 推不出 prefix，
不设 `PYTHONHOME` 时报 `ModuleNotFoundError: No module named 'encodings'`（不是缺标准库）。
`setup.bat` 会设好它，所以 `ipm_gen_soc.py` 那条不用另设。

## 加新工具的规则

- 放进上面三类之一；三类都不合适时再开新目录，并在本表登记。
- 只做一次的实验脚本、临时诊断程序不进 `tools/`。实验程序放 `sw/apps/probe/`，
  一次性的核对做完就删，结论与过程写进 `Engineering/TechReports/`。
- 不写绝对路径：工具位置按环境变量或相对本脚本的位置推，仓库根由脚本位置推出，不要求从特定目录运行。
- 判断成功要有正面证据（输出里的成功标记、字节数、`done`），不能只看子进程的退出码：
  厂商脚本出错时也可能返回 0，见 `board/README.md`。
- 判据脚本要带注错：把一处改错，判据必须报 FAIL，只跑正常用例说明不了判据在检查东西。
