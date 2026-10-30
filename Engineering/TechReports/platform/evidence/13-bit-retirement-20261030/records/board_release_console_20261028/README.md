# board_release_console_20261028

统一PC与RTOS应用菜单；当前软件选定验收通过，固定1/2/3/4、Esc返回、C切场景。B7复用冻结位流，source.zip中的当前在途RTL快照不代表已综合验收。仅JTAG，未写Flash；HDMI与实体输入观感待用户检阅。

双击 `Run-PC.cmd` 启动原生 Windows 游戏；双击 `Run-Board.cmd` 重新加载本包 bit/BIN 并打开控制面板。
`Run-PC-Fullscreen.cmd` 直接全屏启动；F11/Alt+Enter切全屏，F1/F2/F3调窗口，F4切平滑/像素放大。
模式4默认自动适应宽屏；固定640×480对照可运行 `Run-PC.ps1 -FixedAspect`。
PC 不需要板卡或 Efinity。板端需要 Efinity 2026.1/IDE、FT4232H 驱动和已装入 Flash 的原配素材包；本包保留 assets.pak。
板端仅 JTAG 重载，不写 Flash。UART 自动定位 C 通道；多板时用 Run-Board.ps1 -Port 指定。
加载前关闭占用同一串口的控制面板；程序退出/断电后可再次运行本包入口恢复。
PC 与板端默认同一应用菜单。1/2/3/4 全局进入 2D演示/弹幕/压力/三维；Enter/Space确认，Esc返回，C切三维场景。
板载四键：左右同按确认，上下同按返回；游戏中确认可使用炸弹。Shift切阶梯，Tab切后端。
`source.zip` 是此包冻结时的源码，`source.diff` 保留当时相对 Git 的变更。
文件身份及旧/新版本的实际构建说明见 manifest.json，运行日志写 runtime/。
