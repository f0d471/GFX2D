# R4 三维演示验证

R4沿用128B记录与B7的30带整帧接口。`test_scene.c`在64/32位检查2000个逻辑tick的
确定性、视口边界、记录合法性、容量拒绝，以及门环删除（2026-11-01）后
门数据不再产生几何的定向场景。场景共用 `rast_ref` 的像素/深度语义，不用墙钟生成几何。
另检查自机所在深度的命中、门环删除后不留隐形墙、击落敌机、炸弹按下沿、L1无隐藏敌机弹，
以及跨120相机相位的墙缓存/强制无缓存、共享顶点/通用投影、视口内快路/通用裁剪的完整记录流一致性。
另有50000组应用投影到R1通用出口的逐记录对照；模型法线和图集均从生成器复核。

从仓库根先打印计划，再执行：

```
python3 Main/tools/check/validate.py --checks r4 --profile acceptance
python3 Main/tools/check/validate.py --checks r4 --profile acceptance --run
```

宿主检查编同一应用的L1取60帧、L2取240帧覆盖开火/门框穿越（茶杯场景2026-11-02删除）；输入使用独立R4RP录像，HUD在功能构建中
明确显示TEST并固定时间/后端字段。检查主表CRC和全帧可见像素CRC，使用图集及采集器反例。
正常库的ASan/UBSan、64/32与数值契约测试仍由lib/rast承担。

物理板测在Windows Efinity Python下选择`r4-board`。它从本机既有构建中寻找RTL身份一致的
B7（也可给`check_r4_board.py --tree`），复核时序、记录JTAG/UART与bit/BIN身份，重新部署。
依次核对L1的60帧、L2的240帧、板上CPU两场景各2帧、录像导出、物理像素注错、模式1/2/3/4与SW/HW/L1/L2
切换，然后执行预热31帧后的1800帧自动驾驶窗口和1800帧脚本移动/炸弹窗口，
以及现有R3的真实错误/复位恢复组。两种输入各自满足完整节拍判据，不合并统计。

`R4CAD frame render_cycles due actual swaps rast_cycles pixel_crc record_crc`仅在功能构建输出。
`R4PERF samples p50 p99 max late exact over_budget fields swaps`使用逐帧u32区间，分位数按最近秩；
正式窗口必须samples=1800、fields=3600、swaps=1800、late/over_budget=0，max≤3333333周期。
`R4HEALTH snap_frame fifo_min zero under resp`检查显示累计错误为0。
`R4TIME logic geom bin hud submit raster bbox refs`记录周期分解、候选点与条带引用；
submit 已包含 raster，不将两者再次相加。墙相位预热在进入计时锚点前完成。
功能CRC/缓存失效不在渲染计时内，功能构建墙钟帧率不作为性能证据；性能构建不做逐帧CRC。

quick只取每场景12帧与120帧性能取样，不代替60秒验收。JTAG加载不代表Flash固化，
HDMI观感与实体按键由人工另行检阅。

Windows原生全屏/比例/F4过滤/纹理重建、真实按下和失焦松键由独立的
`validate.py --checks pc-window --profile acceptance --run`检查；它需要当前用户桌面。
单靠SDL逻辑事件不能证明Windows物理键盘状态，测试先确认自身前台，再发送扫描码并始终释放。
