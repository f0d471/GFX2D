本次最终审阅部署证据

- manifest.json：位流、BIN、当前输入与四组正式运行的身份。
- logs/：首次部署与取样后重新启动的原始串口、加载边界和写入字节数。
- qa/：初版/HTML目标/当前参考的对照、当前L1/L2/茶杯参考图、控制面板截图。
- qa/metadata.json：参考帧号/CRC及PNG身份，图像传输逐字节核对。
- panel-initial.json/panel-final.json：本次面板真实反馈；用户可继续切换模式。
- debug/：20fps到30fps的诊断候选串口、PC首轮失败汇总与同BIN独立复测、未提交改动保留核对。

当前参考图是宿主光栅输出，已经与本次板上CRC对账；不是HDMI实拍。
HTML截图为浏览器模拟参考，面板截图为Windows实际窗口。
debug候选没有冻结完整源码，只解释定位过程，不替代正式验收。
JTAG加载不等于Flash固化；HDMI观感和实体按键仍待用户审阅。
