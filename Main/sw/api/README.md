# 公开平台 API

应用从 `platform.h` 开始，只需要本目录和标准 C。两个完整消费者在
`../apps/examples/pixels` 与 `../apps/examples/bounce`，不依赖旧游戏、BSP 或内核头。

```sh
cd Main/sw/apps/examples/pixels
make PLATFORM=host
./build_host/sdk-pixels
```

初始化后获取帧句柄，map取得CPU像素与pitch，unmap结束访问，present转交所有权，
wait确认显示完成。旧句柄不可重用；提交前失败仍归调用者，成功提交后不能release。
显示超时保留未知所有权并进入fault，不能靠软件清标记回收。shutdown后不能再获取。
服务单实例、单任务所有者、非ISR安全；一次固件运行不能与旧低层显示入口混用。
该约束由busy/代次/状态检查执行，不承诺C代码越界写的内存隔离。

`platform_time_ms`是初始化后的64位单调毫秒；输入提供held/pressed/released。
资源读取有偏移/长度边界；存储记录上限4096字节，名称仅允许字母数字下划线短横线，
PC必须设置`GFX2D_STORE_DIR`到已存在的目录。双槽、版本、序号和CRC保证可识别中断写；
PC故障注入用`GFX2D_STORE_FAIL_AFTER`。操作返回IO后应读回确认实际有效版本。
目标未获分区时返回NOT_SUPPORTED，不偷偷写入程序或素材区域。

自研加速器的低层ABI属于图形运行时和诊断接口，不能直接等同于SDL/OpenGL兼容层。
CPU生成整帧像素的第三方游戏可接显示/输入/时间及所需存储适配，接入不自动迁移其
内部光栅运算到FPGA。完整职责、故障语义和证据边界见[接口契约](../../doc/平台软件接口.md)。
