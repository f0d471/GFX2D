# 固件组合与调度入口

`console/main_host.c`与`main_rtos.c`把同一内容配置接到宿主循环/FreeRTOS runner。
`console/FreeRTOSConfig.h`属于这份固件配置；内核源码在只读sdk的软件组件中，
`sw/rtos`只实现本板trap/PLIC与显示、引擎两类事件桥。
`legacy/main.c`保留原开发/判据入口，通过game/gfx2dGame的makefile构建。

内容代码不创建板级中断、处理CLINT地址或配置内核。系统端口与固件启动可以使用
操作系统和BSP。PC模型与交叉编译均不能替代目标运行或上电Flash启动验证。
