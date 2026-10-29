# 系统与图形服务

`platform/`实现公开SDK的帧所有权、输入/时间/资源与持久化入口；`gfx/`实现自研
加速器的通用renderer、资源窗口与平台setup选择；`input/`实现产品输入映射策略。
这里可以使用BSP与纯库，不允许依赖游戏、场景或注册表。

`test/`在PC上编译真实服务、实际引擎驱动、实际调度端口和中断桥，使用可控设备/内核
接口替身验证错误与完成语义。它不模拟整个FreeRTOS，也不证明硬件IRQ/Flash已运行。
唯一检查入口为`validate.py --checks platform-sdk --profile acceptance --run`。
