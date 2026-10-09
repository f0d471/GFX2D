# soc：重新生成 SoC IP

| 文件 | 用途 |
| --- | --- |
| `ipm_gen_soc.py` | 用 IP Manager 的官方 Python API 完整生成 Sapphire SoC：包装文件 `soc.v`、网表、片上 RAM 初始文件、软件包 |

完整步骤（登记项恢复、回归）见 [`../../sdk/README.md`](../../sdk/README.md)「重新生成 SoC IP 时」，这里只讲工具本身。

## 用法

```
wsl -e python3 Main/tools/check/localize_paths.py --apply
cmd /c "call <EFINITY>\bin\setup.bat && python Main\tools\soc\ipm_gen_soc.py --project Main\fpga\gfx2d_ti60 --sync-sdk Main\sdk"
```

| 参数 | 说明 |
| --- | --- |
| `--project` | Efinity 工程目录：主线工程，或仓库外的复制工程（例如 E8 比较缓存大小时） |
| `--override K=V ...` | 覆盖 `settings.json` 里的参数，例如 `DCacheSize=8192`；键名不存在直接报错，不会静默无效 |
| `--sync-sdk <目录>` | 生成后把软件包复制到这个目录并删掉 `embedded_sw/`。主线给 `Main\sdk`，复制工程不给 |
| `--dry-run` | 只打印参数对照与文件集，不生成。改参数前先跑一次 |
| `--outputs` | 文件集，默认 `generator source script embedded_sw`，不生成示例工程与仿真文件集 |

`--dry-run` 会列出两份清单：`settings.json` 里有而新版本 IP 里没有的参数，
新版本 IP 里有而 `settings.json` 里没有、只能取默认值的参数。升级 IP 版本时两份都要看。

## 生成之后脚本替你做的事

1. **补回文件清单。** API 把 `settings.json` 里 `output.external_source_source` 写成空列表
   （图形界面生成时记着 `soc.v` 等四个源文件）。生成前非空、生成后变空的清单按原文补回，原格式与行尾不变。
2. **删掉示例目录。** `ip/soc/` 下新冒出来的 `T120F324_devkit/`、`Ti60F225_devkit/`、`Testbench/` 不入库。
3. **同步软件包**（给了 `--sync-sdk` 时）。覆盖式复制，生成器不产的文件（`sdk/README.md`、512KB bootloader）不受影响；
   `sdk/README.md` 登记的修改会被冲掉，要逐条恢复。
4. **固定哈希种子。** 生成器按 `pyrsistent` PMap 的遍历顺序写 `soc_config`，这个顺序随 Python 每个进程的随机哈希种子变，
   同一份配置每次生成出来的 `soc_config` 选项顺序、`soc.h` 的行序、网表里 GPIO0 与 GPIO1 的内部编号都不一样。
   功能等价，但网表整片改动，会扰动布局。脚本给生成器子进程设 `PYTHONHASHSEED=0`，
   2026-09-19 在复制工程里连续生成两次，结果只差 `soc.v` 里的 `IP_UUID` 一处。

## 运行环境的两个坑

- **必须在 `setup.bat` 的环境里跑。** 否则 `devicedb` 的 DLL 加载失败：`OSError ... libdevicedb_wrapper.dll: error 0x7e`。
- **工程要与 bootloader hex 在同一个盘。** `settings.json` 的 `HexFile_Path` 指向 `Main/sdk/tool/bootloader/`，
  复制工程放在别的盘上时生成器的 `relpath` 跨盘报错。复制工程放在仓库外、与仓库同一个盘的目录里。
  `HexFile_Path` 必须先用 `localize_paths.py --apply` 展开，还是占位符时脚本直接报错。

## 缓存候选复制工程

make_cache_variant.py 复制当前工程，重定位 RTL 引用，调用 ipm_gen_soc.py
覆盖 DCacheSize/DCACHEWAY；不修改主线 IP 或 SDK。默认候选为
Main/sim/engine/g4/build/cache16/gfx2d_ti60，16KB/4 路。

~~~text
cmd /c "call <EFINITY>\bin\setup.bat && python Main\tools\soc\make_cache_variant.py --build"
~~~

--dry-run 复制并验证参数，不生成 IP；--output 可指定另一个空复制目录。
已有候选目录默认拒绝重写，确需重建时显式给 --overwrite，重建后按新的
传输边界重新登记部署产物。--build 运行四步并保存 outflow/g4_synthesis.log，
资源时序由 check_g4_integration.py --synthesis <工程> --tag <标签> 收集。
