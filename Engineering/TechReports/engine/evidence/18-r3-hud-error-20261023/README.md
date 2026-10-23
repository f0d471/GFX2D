# 混合帧旧预取与R3排空

`before.log`明确报出EARLY LEGACY ABORT，`after.log`正常结束。
测试先执行3D，再执行20条2D记录的首条非法操作码；合法AR反压让下一批预取保持在途。
旧门控提前启动mover drain，FE还在等待的流会被吞；修正后等旧FE完成再排空，
最后得到code1/index0/tagABCDEF01/stage0，所有单元安静。

保留测试台及改动前后完整命令层源码，不覆盖原输出。
原执行脚本在`Main/sim/build/r3-dev/prove_legacy_abort.py`位置运行，
归档副本须复制回该忽略目录再执行，或调整其根目录解析。
当前源码已修复，复现红例须在脚本生成的隔离副本中用`engine_cmd_before.v`，
绿例用`engine_cmd_after.v`；不得回退工作区生产RTL。
完整永久回归已并入`Main/sim/engine/r3/tb_r3.v`，日常使用注册的`r3`检查。
