# G3：条带控制与软件分桶

正式入口为 tools/check/check_g3_stripctl.py，使用生产 list/strip/exec_sw 与
完整 engine_top。研究模型和原型同时保留，结论与验收边界见
[engine 第 09 章](../../../../Engineering/TechReports/engine/09-条带控制与ABIv2.md)。

## 正式复现

从仓库根，在 WSL 中运行：

~~~text
python3 Main/tools/check/check_g3_stripctl.py
~~~

需要 Python 3、gcc、iverilog/vvp。默认运行 sanitizer 与普通构建各 10000 张
随机同批绘制调用对拍（0 跳过），软件错误边界、四种生产变异，完整引擎
16/32 行同表金标准与三种 RTL 变异。--software-only / --rtl-only 可分段运行。
--collect-syn 采集独立和主工程最新报告，检查四步、全部 slack 与 hold 门槛；
--collect-board 检查原始板测日志、CRC、fail/done 和扫描压力，不烧写。

构建工件和共享内存镜像进入忽略的 build/；证据进入 logs/。
条带表计数、布局、局部坐标与错误约束以 spec 第 02 章 3.9 节为准。

## 原始研究复现

~~~text
python3 Main/tools/check/g3_research.py --json
mkdir -p Main/sim/engine/stripctl/build
gcc -std=c11 -I Main/sw/lib -fsanitize=address,undefined -g \
    Main/sim/engine/stripctl/tiling_fuzz.c \
    Main/sw/lib/gfx/check.c Main/sw/lib/gfx/list.c Main/sw/lib/gfx/strip.c \
    Main/sw/lib/gfx/exec_sw.c Main/sw/lib/core/format.c \
    -o Main/sim/engine/stripctl/build/tiling_fuzz
Main/sim/engine/stripctl/build/tiling_fuzz --json
~~~

研究模型按游戏几何口径重放桶分布和命令取数开销；原型为 1000 张、四种变异，
分桶与软件窗口仍在原型内部。正式判据使用生产分桶库，不用原型替代。

## 工件和证据

| 文件 | 内容 |
| --- | --- |
| scene_dump.c | 同批调用产生直通金标准、生产 v2 表和 RTL DDR 初值；完整帧含 padding |
| test_strips.c | 生产库 10000 张，跨桶、两次 clip、负坐标、色键和六方程 |
| test_errors.c | 错误三元组、原子溢出、1024/1025、空面积早检与坐标编码 |
| tb_stripctl.v | 完整 engine_top，空桶、批中 FETCH、总线错误、超时、排空、软复位、PERF |
| logs/formal_results.json | 软件/RTL 正式结果汇总 |
| logs/regression_results.json | 十三套既有仿真结果 |
| logs/formal_synthesis_results.json | 独立/整机资源与时序，最新编译段告警 |
| logs/formal_board_results.json | 同帧 CRC、错误恢复与旧 engineTest 兼容性 |
| logs/board_artifacts.json | 板测 bit/bin 身份唯一登记 |
| logs/acceptance_review.json | 验收复核对照，引用以上原始结果 |
| logs/research_results.json、fuzz_results.json | 研究模型、1000 张原型与四种变异 |

板测程序为 sw/apps/test/stripTest，经 board_run.py 的 --bin 走 JTAG；
完整工具命令及本机占位符见 doc/开发环境.md。板卡证据超出 G3 的原定最低边界，
仍是诊断场景；G4 的游戏条带接入、持续 N_max、cache 对照和作品数字尚未验收。
