# assets：素材的生成与打包

素材流水线的两个工具，输入是 `Main/assets/` 下的 PNG 与点阵字库文本，输出是进构建的素材包与头文件。

| 文件 | 用途 |
| --- | --- |
| `gen_placeholder.py` | 生成占位 PNG（纯色、棋盘、渐变、圆形弹幕、色键图、图集）。图案全部按坐标算出来，没有随机数，同一个 Pillow 版本产出逐字节相同的 PNG |
| `gen_stg_neon.py` | 生成游戏的霓虹几何素材：三种弹各四色、自机弹、火花、自机与两种敌机、Boss、星点、标题（含 EX 提示行）、大号数字、图标、青色与琥珀字库、背景合图，共 14 张 PNG。图案按坐标算出，不用随机数与字体文件。其中 8 张入库的是 `apply_kenney_art.py` 的叠加版：写回入库目录时默认跳过、`--check` 不比，加 `--base` 才连它们一起写回基线；`--check` 重新生成并与入库的逐像素比，`--check --inject` 改一个像素必须见红 |
| `apply_kenney_art.py` | 在霓虹基线上换掉 8 张：自机、两种敌机、Boss、自机弹、残机图标取 Kenney Space Shooter Redux（CC0）原图改色缩放，圆弹、米弹、背景楼群按坐标重画。`--src` 指向解包后的 Kenney PNG 目录（原图不入库） |
| `gen_font_txt.py` | 把原样入库的上游点阵字体 `Main/assets/font/spleen-8x16.bdf`（Spleen 8x16，BSD-2-Clause）转换成打包工具读的字库文本 `font/ascii8x16.txt`，只取 ASCII 的 95 个字形；`--check` 重新生成并与入库的逐字节比，`--check --inject` 改一个像素必须见红 |
| `pack_assets.py` | 读 `Main/assets/manifest.txt`，把每一项转成目标字节、打成 `assets.pak`，并生成 `assets.h` |

```
wsl -e python3 Main/tools/assets/gen_placeholder.py            # 生成占位 PNG（产物入库）
wsl -e python3 Main/tools/assets/gen_placeholder.py --check    # 只校验现有 PNG 与预期像素一致
wsl -e python3 Main/tools/assets/gen_stg_neon.py               # 生成游戏素材到 Main/assets/neon/（产物入库；Kenney 叠加的 8 张跳过）
wsl -e python3 Main/tools/assets/gen_stg_neon.py --check       # 重新生成，与入库的逐像素比
wsl -e python3 Main/tools/assets/gen_stg_neon.py --base        # 8 张也写回基线，之后必须接着跑下一行
wsl -e python3 Main/tools/assets/apply_kenney_art.py --src <Kenney PNG 目录>   # 在基线上叠 Kenney 层
wsl -e python3 Main/tools/assets/gen_font_txt.py               # 由 spleen-8x16.bdf 生成字库文本（产物入库）
wsl -e python3 Main/tools/assets/gen_font_txt.py --check       # 重新生成，与入库的逐字节比
wsl -e python3 Main/tools/assets/pack_assets.py                # 打包到 Main/assets/build/
wsl -e python3 Main/tools/assets/pack_assets.py --max-pak 1048576    # 收紧素材包的体积预算
```

## 目录约定

```
Main/assets/
  manifest.txt         素材清单（TSV），显式 ID + 名字 + PNG + alpha_mode + 色键 + 切片
  placeholder/*.png    由 gen_placeholder.py 生成，入库
  neon/*.png           由 gen_stg_neon.py 生成（其中 8 张再由 apply_kenney_art.py 叠 Kenney 层），入库，并在 LICENSES.md 里逐个登记
  font/spleen-8x16.bdf 上游点阵字体 Spleen 8x16（原样入库，许可全文在 font/LICENSE-spleen.txt）
  font/ascii8x16.txt   由 gen_font_txt.py 从上面的 BDF 转换的 8×16 ASCII 字库（95 个字形），入库，打包工具渲染成图集
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

`Main/tools/check/check_sw5_assets.sh` 把素材流水线的软件侧判据串起来跑（含七种坏包注错、
一次注错构建、镜像回环与许可登记检查），另有一份独立实现 `check_sw5_pak.py` 自己解包、
自己从 PNG 与字库重算每一项的像素字节再逐字节比——打包与解包不能互相证明。
