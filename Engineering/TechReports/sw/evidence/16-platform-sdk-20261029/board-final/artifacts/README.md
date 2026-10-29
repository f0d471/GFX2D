# SW16 板端验证输入归档

本目录保留实际验收的B7位流、各测试BIN、正常console ELF、调试配置及接受时软件源码。
摘要和来源见上一级retained-artifacts.json；原始日志和装载边界在上一级logs。
JTAG验证不写Flash，现有素材位于板上原Flash布局；本目录不是Flash启动更新包。

如需重载本次正常菜单，从仓库根在PowerShell执行：

```powershell
$sw16Artifacts = 'Engineering/TechReports/sw/evidence/16-platform-sdk-20261029/board-final/artifacts'
& .\Main\tools\board\python-efinity.cmd Main/tools/board/board_run.py --bit "$sw16Artifacts/hardware/gfx2d_ti60.bit" --bin "$sw16Artifacts/sw16-target/console-normal/console-O2-perf.bin" --precise-boundary --seconds 60 --log Main/sim/build/sw16-review.serial.log
```

首次复现应按清单核对文件SHA256；该命令会重新配置板卡，先确保串口/JTAG未被其他程序占用。
源码ZIP不包含未改动的厂商SDK或已有素材；完整构建仍使用仓库配套SDK/素材和验证入口。
