# Main/assets 的来源与许可登记表

判据：`Main/assets/` 下的**每个非生成文件**都必须在本表里有登记；
`Main/tools/check/check_sw5_assets.sh` 会按这条扫一遍（有文件没登记就报红）。

「生成文件」指由仓库里的脚本产出、可以按脚本重新生成的文件，不需要登记：
`placeholder/*.png` 由 `Main/tools/assets/gen_placeholder.py` 生成，
`build/*`（`assets.pak`、`assets.h`）由 `Main/tools/assets/pack_assets.py` 生成。
它们本身也没有第三方来源——图案是按坐标算出来的几何图形。

| 文件 | 来源 | 许可 | 说明 |
| --- | --- | --- | --- |
| `manifest.txt` | 本项目编写 | 项目自有 | 素材清单（TSV），打包工具的输入 |
| `font/ascii8x16.txt` | 脚本生成（`Main/tools/assets/gen_font_txt.py` 由 `font/spleen-8x16.bdf` 转换） | BSD-2-Clause，随 Spleen（Copyright (c) 2018-2026, Frederic Cambus） | 8×16 点阵 ASCII 字库，95 个字形（0x20~0x7E）；打包工具把它渲染成图集。产物入库，`check_sw7_game.sh` 由 BDF 重新生成并逐字节比对 |
| `font/spleen-8x16.bdf` | 上游原样：Spleen 2.2.0 的 `spleen-8x16.bdf`（https://github.com/fcambus/spleen），未修改 | BSD-2-Clause，Copyright (c) 2018-2026, Frederic Cambus，全文见 `font/LICENSE-spleen.txt` | 字库的像素来源；只取其中 ASCII 的 95 个字形 |
| `font/LICENSE-spleen.txt` | 上游原样：Spleen 的 `LICENSE` | BSD-2-Clause 原文 | 分发字体与由它渲染的像素时必须附带的版权声明与许可全文 |
| `LICENSES.md` | 本项目编写 | 项目自有 | 本表 |
| `neon/bul16.png` | 脚本生成：`gen_stg_neon.py` 基线上由 `Main/tools/assets/apply_kenney_art.py` 按坐标重画 | 项目自有，无第三方像素 | 16×16 圆弹，四色，白芯 + 亮环 + 外辉光；产物入库，`gen_stg_neon.py --check` 不比（叠加脚本要 Kenney 包才能重跑） |
| `neon/bul8.png` | 脚本生成：`gen_stg_neon.py` 基线上由 `Main/tools/assets/apply_kenney_art.py` 按坐标重画 | 项目自有，无第三方像素 | 8×8 小圆弹，四色；产物入库，`gen_stg_neon.py --check` 不比 |
| `neon/rice20.png` | 脚本生成：`gen_stg_neon.py` 基线上由 `Main/tools/assets/apply_kenney_art.py` 按坐标重画 | 项目自有，无第三方像素 | 20×20 米弹，四色 × 16 个方向，椭圆截面胶囊；产物入库，`gen_stg_neon.py --check` 不比 |
| `neon/pshot.png` | 派生：Kenney Space Shooter Redux 的 `Lasers/laserBlue01.png`，经 `apply_kenney_art.py` 转青色、缩到 6×24 | CC0（Kenney.nl） | 自机弹光束；产物入库，`gen_stg_neon.py --check` 不比 |
| `neon/spark.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 8×8 火花，4 帧 × 5 档亮度；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/ships.png` | 派生：Kenney Space Shooter Redux 的 `playerShip1_blue`、`Enemies/enemyRed2`、`Enemies/enemyGreen2`，经 `apply_kenney_art.py` 改色、缩放、做摆动帧 | CC0（Kenney.nl） | 32×32：自机 1 帧、DART 4 帧、CROSS 4 帧；产物入库，`gen_stg_neon.py --check` 不比 |
| `neon/boss.png` | 派生：Kenney Space Shooter Redux 的 `Enemies/enemyBlack3`，经 `apply_kenney_art.py` 暗部染暗品红、缩到 74px、转出 6 帧 | CC0（Kenney.nl） | 96×96 Boss，6 帧（2 列 × 3 行）；产物入库，`gen_stg_neon.py --check` 不比 |
| `neon/stars.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 2×2 星点 4 种；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/title.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 240×90 标题 MYRIAD（笔画按线段画，不用字体文件；2026-09-27 定案去掉中文副标题）；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/digits.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | BSD-2-Clause，随 Spleen（字形来自 `font/ascii8x16.txt`） | 16×32 大号数字，由 `font/ascii8x16.txt` 放大一倍；产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/icons.png` | 脚本生成（`gen_stg_neon.py`），第 0 格由 `apply_kenney_art.py` 换成 Kenney `playerShip1_blue` 缩图 | 第 0 格 CC0（Kenney.nl），其余项目自有 | 16×16 图标：自机、炸弹、判定点；产物入库，`gen_stg_neon.py --check` 不比 |
| `neon/font_cyan.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | BSD-2-Clause，随 Spleen（字形来自 `font/ascii8x16.txt`） | 青色 8×16 字库，照 `font/ascii8x16.txt` 渲染；产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/font_amber.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | BSD-2-Clause，随 Spleen（字形来自 `font/ascii8x16.txt`） | 琥珀 8×16 字库（R4 HUD 数值），照 `font/ascii8x16.txt` 渲染；产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/bg_layers.png` | 脚本生成：天空区由 `gen_stg_neon.py` 生成，远景带与城市带由 `apply_kenney_art.py` 按坐标重画（32/64px 一栋） | 项目自有，无第三方像素 | 512×608 背景合图；产物入库，`gen_stg_neon.py --check` 不比 |

## 字库的来源

2026-09-22 的第一版是自绘的：当时本仓与厂商资料包里都没有字库文件，自绘省掉了核对许可与离线可得两件事。
自绘版有几个数字与符号的字形残缺（`0` 是实心块、`1` 像下划线），游戏 HUD 的数字难认，
2026-09-27 定案换成现成的开源点阵字体 Spleen 8x16：

- 许可是 BSD-2-Clause，只要求保留版权声明与许可全文；两者已随字体放进 `font/`。
- 上游文件原样入库（`font/spleen-8x16.bdf`），由 `Main/tools/assets/gen_font_txt.py` 转换成打包工具读的
  `font/ascii8x16.txt`，离线可以重新生成；打包工具、霓虹素材脚本与判据侧的 HUD 独立重画都只读这份文本，接口不变。
- 字库的像素来源就是 `font/spleen-8x16.bdf` 这一个上游文件，转换规则写在脚本的文件头里。

BSD-2-Clause 对二进制分发的要求：素材包与 Flash 镜像里带着由它渲染的像素，
作品随附的文档（报告或说明）里要附上 `font/LICENSE-spleen.txt` 的版权声明与许可条文。

**没有登记许可的非生成素材不入库。** 正式角色、弹幕与背景的美术生产随游戏主题的选择另排，
到时加进来的每一份非生成素材都要先在本表登记。

正式素材（`neon/` 下 14 张 PNG）的基线由 `Main/tools/assets/gen_stg_neon.py` 按坐标生成，其中 8 张入库的是
`apply_kenney_art.py` 叠过 Kenney 贴图或重画的版本（见下节）；青色、琥珀字库与大号数字照 `font/ascii8x16.txt` 渲染，许可随 Spleen。它们在上表逐个登记，不走「生成文件不登记」那一条。

## 第三方 CC0 素材的派生（2026-10-31 起补记）

- **Kenney Space Shooter Redux**（2D，CC0，opengameart.org/content/space-shooter-redux）：
  `neon/` 下 ships/boss/pshot/icons 四张取其原图，由 `Main/tools/assets/apply_kenney_art.py` 改色缩放派生；
  上表已按实际来源逐个登记（2026-11-01 订正）。原图包不入库，下载地址即本条。
- **Quaternius Ultimate Spaceships**（3D，CC0，opengameart.org/content/lowpoly-spaceships-pack）：
  三维模式的 `r4_models.h`（自机 Challenger、敌机 Omen，经聚类减面）由
  `Main/tools/assets/gen_r4_models_obj.py` 从其 OBJ 派生；
  `assets/r4/atlas.png` 的敌机 billboard（Q2 区块）按同一模型剪影重画。
- **Kenney Space Kit 3D**（CC0，opengameart.org/content/space-kit）：
  `r4_models.h` 曾用其 spaceCraft 系模型（2026-10-31 起换 Quaternius，留档备查）。

CC0 无署名义务，本节登记只作溯源。
