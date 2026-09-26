# Main/assets 的来源与许可台账

判据：`Main/assets/` 下的**每个非生成文件**都必须在本表里有登记；
`Main/tools/check/check_sw5_assets.sh` 会按这条扫一遍（有文件没登记就报红）。

「生成文件」指由仓库里的脚本产出、可以按脚本重新生成的文件，不需要登记：
`placeholder/*.png` 由 `Main/tools/assets/gen_placeholder.py` 生成，
`build/*`（`assets.pak`、`assets.h`）由 `Main/tools/assets/pack_assets.py` 生成。
它们本身也没有第三方来源——图案是按坐标算出来的几何图形。

| 文件 | 来源 | 许可 | 说明 |
| --- | --- | --- | --- |
| `manifest.txt` | 本项目编写 | 项目自有 | 素材清单（TSV），打包工具的输入 |
| `font/ascii8x16.txt` | 本项目自绘 | 项目自有，无第三方来源 | 8×16 点阵 ASCII 字库，95 个字形（0x20~0x7E），逐像素手绘；打包工具把它渲染成图集 |
| `LICENSES.md` | 本项目编写 | 项目自有 | 本台账 |
| `neon/bul16.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 16×16 圆弹，四色；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/bul8.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 8×8 小圆弹，四色；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/rice20.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 20×20 米弹，四色 × 16 个方向；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/pshot.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 6×24 自机弹光束；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/spark.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 8×8 火花，4 帧 × 5 档亮度；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/ships.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 32×32 线框：自机、DART、CROSS；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/boss.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 96×96 Boss，12 帧；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/stars.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 2×2 星点 4 种；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/title.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 240×90 标题 MYRIAD 与「万弹」（笔画按线段画，不用字体文件）；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/digits.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 16×32 大号数字，由 `font/ascii8x16.txt` 放大一倍；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/icons.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 16×16 图标：自机、炸弹、判定点；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |
| `neon/font_cyan.png` | 脚本生成（`Main/tools/assets/gen_stg_neon.py`） | 项目自有，无第三方来源 | 青色 8×16 字库，照 `font/ascii8x16.txt` 渲染；图案按坐标算出，产物入库，`check_sw7_game.sh` 重新生成并逐像素比对 |

## 为什么字库是自绘的

2026-09-22 查过本仓与厂商资料包：`Main/` 下没有任何点阵或矢量字库，
`Efinix/`（7 GB 厂商资料包）下也没有 `.ttf`/`.bdf`/`.fnt` 之类的字库文件。
选外部 CC0 字库要额外核对许可、还要保证离线可得，自绘把这两件事一起去掉。
字库的像素来源就是 `font/ascii8x16.txt` 这一个文本文件，可逐字复核。

**没有登记许可的非生成素材不入库。** 正式角色、弹幕与背景的美术生产随 SW7 的主题选择另排，
到时加进来的每一份非生成素材都要先在本表登记。

SW7 C 批的正式素材（霓虹几何，`neon/` 下 12 张 PNG）全部由 `Main/tools/assets/gen_stg_neon.py` 按坐标生成，
不引入第三方素材与字体文件；它们在上表逐个登记（SW7 2.7 节的定法），不走「生成文件不登记」那一条。
