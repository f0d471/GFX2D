# pbTest

F5 正式板上判据：DDR/TEX、六方程、XY4/XYUV8 与跨 4 KiB 的 stride=12，
对独立标量软件执行器比较像素及行 padding；PB 目录、metadata、引用按字节
与显式窗口模型对账。覆盖容量拒绝、包错误定位和 v1/PB 模式往返。

从仓库根运行 `Main/tools/check/run_f5_board.py --build tests --run tests --program`。
只经 JTAG 装载比特流和 DDR 程序，不写 Flash。
