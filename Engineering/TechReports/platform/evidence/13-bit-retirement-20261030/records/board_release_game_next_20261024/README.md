# board_release_game_next_20261024

Software-only visual upgrade. No RTL or Flash modification. Native Windows: PERF=1 START_MODE=4 R4_HW=0; board: PERF=1 START_MODE=4 START_BACKEND=2 R4_HW=1 R4_LADDER=2. Both unlimited. 480x480 world, atlas v3, scene 0x52344334. B7 byte-identical to tested bit. See verification.json for exact acceptance scopes and the packaging-only PS5.1 hash fix.

双击 `Run-PC.cmd` 启动原生 Windows 游戏；双击 `Run-Board.cmd` 重新加载本包 bit/BIN 并打开控制面板。
`Run-PC-Fullscreen.cmd` 直接全屏启动；F11/Alt+Enter切全屏，Esc退回窗口，F1/F2/F3调窗口，F4切平滑/像素放大。
PC 不需要板卡或 Efinity。板端需要 Efinity 2026.1/IDE、FT4232H 驱动和已装入 Flash 的原配素材包；本包保留 assets.pak。
板端仅 JTAG 重载，不写 Flash。UART 自动定位 C 通道；多板时用 Run-Board.ps1 -Port 指定。
加载前关闭占用同一串口的控制面板；程序退出/断电后可再次运行本包入口恢复。
按 4 进入三维、三维内再按 4 切茶杯；Shift 切 L1/L2，Tab 切软/硬件；WASD/方向键移动，Space 炸弹。
`source.zip` 是此包冻结时的源码，`source.diff` 保留当时相对 Git 的变更。
文件身份及旧/新版本的实际构建说明见 manifest.json，运行日志写 runtime/。
