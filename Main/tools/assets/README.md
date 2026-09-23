# assets：素材的生成与打包

SW5 素材流水线（[`../../../Engineering/Todo/sw/SW5-素材流水线.md`](../../../Engineering/Todo/sw/SW5-素材流水线.md)）
的两个工具，输入是 `Main/assets/` 下的 PNG 与点阵字库文本，输出是进构建的素材包与头文件。

| 文件 | 用途 |
| --- | --- |
| `gen_placeholder.py` | 生成占位 PNG（纯色、棋盘、渐变、圆形弹幕、色键图、图集）。图案全部按坐标算出来，没有随机数，同一个 Pillow 版本产出逐字节相同的 PNG |
| `pack_assets.py` | 读 `Main/assets/manifest.txt`，把每一项转成目标字节、打成 `assets.pak`，并生成 `assets.h` |

```
wsl -e python3 Main/tools/assets/gen_placeholder.py            # 生成占位 PNG（产物入库）
wsl -e python3 Main/tools/assets/gen_placeholder.py --check    # 只校验现有 PNG 与预期像素一致
wsl -e python3 Main/tools/assets/pack_assets.py                # 打包到 Main/assets/build/
wsl -e python3 Main/tools/assets/pack_assets.py --max-pak 1048576    # 收紧运行预算（E6 出数后用）
```

## 目录约定

```
Main/assets/
  manifest.txt         素材清单（TSV），显式 ID + 名字 + PNG + alpha_mode + 色键 + 切片
  placeholder/*.png    由 gen_placeholder.py 生成，入库
  font/ascii8x16.txt   自绘 8×16 点阵 ASCII 字库（95 个字形），打包工具渲染成图集
  LICENSES.md          每个非生成文件的来源与许可（判据脚本会按这条扫）
  build/               生成物：assets.pak、assets.h（.gitignore 已排除 build/）
```

`assets.h` 与 `assets.pak` 都**不入库**：改了 manifest 或 PNG 之后重跑打包即可。
`build/` 这一条路径被 `mk/*.mk` 的 `ASSETS_BUILD` 用到——只有**应用**那一层的编译规则
带 `-I$(ASSETS_BUILD)`，`lib/` 与 `bsp/` 都看不到它（分层不变）。

## 打包工具的两条硬约束

1. **预乘必须用 65536 项查表**，口径 `(c * a + 127) / 255`。Pillow 的 `ImageChops.multiply`
   是 `floor(c * a / 255)`，65536 组里有 31770 组与规格不同（48.5%），不能替换查表实现。
2. **相同输入逐字节产出相同的包与头文件**：条目顺序就是 manifest 顺序，产物里没有时间戳、
   没有绝对路径、没有随机数。`check_sw5_assets.sh` 的 S1 就是打两次比逐字节。

只接受能按规格展开成 8 位 RGBA 的 PNG 模式（`1`、`L`、`LA`、`P`、`RGB`、`RGBA`）：
Pillow 对 16 位灰度 `I;16` 的 `convert("RGBA")` 是**截断到 255**（1000 → 255），不是缩放，
静默转换会产出一张近全白的图，到画面上才发现。工具同时检查文件内容确实是 PNG，
不能只靠扩展名；素材名转成大写 C 宏后仍须唯一。色键只与 `xrgb` 搭配；v1 不产出 `INDEX8`。

## 判据

`Main/tools/check/check_sw5_assets.sh` 把 SW5 第 3 节的软件侧判据串起来跑（含七种坏包注错、
一次注错构建、镜像回环与许可台账），另有一份独立实现 `check_sw5_pak.py` 自己解包、
自己从 PNG 与字库重算每一项的像素字节再逐字节比——打包与解包不能互相证明。
