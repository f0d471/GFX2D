# R3 本地 JTAG 复现包

B7.bit：CAPS=D46F；rastTest.bin：11帧软硬CRC、显示健康、真实索引错误与复位恢复；
featureTest.bin：按运行时能力执行旧二维功能。字节身份见identity.json。

从仓根运行（串口按开发环境手册及`--list-ports`确认）：

```powershell
Main/tools/board/python-efinity.cmd Main/tools/board/board_run.py --bit Main/bit/board_release_r3_20261023/B7.bit --bin Main/bit/board_release_r3_20261023/rastTest.bin --port <COM> --stop-line done --seconds 240 --log Main/sim/build/r3-repro/serial.log
```

本包不写Flash，默认构型仍B0；正常程序最后呈现完整三维测试帧。
这些是本机构建产物，目录保持忽略，不代替源码与正式证据。
