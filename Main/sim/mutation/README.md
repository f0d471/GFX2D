# 外部变异测试

生产 RTL/C 只保留真实逻辑。`mutants.yaml` 登记缺陷模型、规格出处、唯一匹配的替换规则、
普通测试命令和具体检查器标签；测试台不知道是否施加变异。研究探针和事务级错误响应仍归各自测试环境。

## 执行

从仓库根、配置好开发环境的 WSL 中运行：

```sh
python3 Main/tools/check/validate.py --checks mutation-tools mutation --profile acceptance
python3 Main/tools/check/validate.py --checks mutation-tools mutation --profile acceptance --run
python3 Main/sim/mutation/run_mutation.py --audit
python3 Main/sim/mutation/run_mutation.py --run --profile quick --ids blend.wrong_eq
```

`validate.py` 默认只显示计划，`--run` 才执行。quick/acceptance/release 传递给普通消费者，
分别采用其已有参数；不通过缩小激励或修改期望结果让变异变绿。

## 隔离与身份

规则默认 `match: literal`，逐字匹配；RTL 规则可显式使用 `match: verilog`，
按代码 token 匹配，忽略空白和注释，保留字符串与运算符的区别。
两种方式都要求唯一命中，零处或多处匹配均失败；不做模糊替换。
目前只有五条 blend 规则使用 token 匹配，其余规则的匹配方式不变。
反演工具使用相同的规则应用函数。

跑器冻结 Git 登记的源文件和实际字节摘要，复制到本轮 `build/mutation/shadow/`；
文件后缀不用于猜测是否复制旧夹具，`.bin` 等真实输入仍按源码清单保留。
不复制历史 build/logs。素材和测试向量在隔离树内现场生成。
复制树通过显式源码清单使用公共 `check_run.Session`，不向上寻找外层仓库冒充输入身份。

所有未变异基线先执行并通过，任何基线失败则不执行变异。每条变异只改变登记的目标，
同一条的多处规则同时生效；结束或异常立即恢复。不同命令具有独立运行目录，
shell 套件构建目录也先清空，禁止按文件时间戳捡取上一条输出。

## 判定与证据

- `KILLED`：普通消费者非零退出，具体检查器标签匹配，且不是准备/编译失败。
- `SURVIVED`：普通消费者成功，未检出。
- `KILLED-WEAK`：只超时、只出现总看门狗或没有匹配到指定检查器。
- `PARTIAL`：多个发现者中只有部分检出。
- `INVALID`：规则失效、基础设施失败或输入不成立。

只有选定条目全部 KILLED 才通过；其余状态返回失败。等价变异应另附独立证明后调整清单，
不能仅因它存活而宣布等价，也不要求人为制造一个等价实例。
证据正则由规格与检查器定义，跑器不从失败输出自动改写自己的期望。

输出进入公共运行目录的 `logs/mutation/`：冻结摘要、每条基线和变异的完整 stdout、
原始套件日志、子进程结果、子会话输入身份、报告及文件摘要。验收后由 validate 统一归档，
不截断日志，不覆盖历史记录。`--keep-shadow` 仅用于诊断，正常结束清理影子树。

## 迁移核对

`verify_migration.py --base-rev 551c2029 --json <输出>` 在 B0/B3/B5/B7 下核对生产展开，
有历史宏的条目同时核对反演；每条规则至少在一个构型下改变展开代码，防止关闭分支造成空证明。
迁移前的一次性剥离工具及生产树还原工具已退役，正常维护只需清单、跑器和普通消费者。
