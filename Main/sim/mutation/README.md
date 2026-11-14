# 变异测试活动

生产源码里不放注错分支。变异体写在本目录的 [`mutants.yaml`](mutants.yaml)，跑器在隔离
影子树里施加一条，再用 `Main/sim/` 与 `Main/tools/check/` 下原有的测试跑，按退出码与
登记的见红位置判定。它回答的问题只有一个：**判据到底有没有在检查东西**。

当前全仓 75 条（RTL 70 条、软件执行器 5 条），生产源码里的 `INJ_*` 开关已归零。

## 文件

| 文件 | 作用 |
| --- | --- |
| `mutants.yaml` | 唯一真相：一条变异一行，含缺陷模型、规格出处、一组字面替换规则、发现它的命令与见红位置 |
| `run_mutation.py` | 跑器：建影子树、施加一条规则、跑基线+变异体、按退出码与正则分类、写报告 |
| `verify_migration.py` | 迁移等价性证明：正向逐行相同、反演词法相同（Verilog 用 `iverilog -E`，C 用 `gcc -E`） |
| `migrate_ifdef.py` | 迁移期一次性工具：把 `INJ_*` 块抽成规则并删除变异分支；全部迁完即可删除 |

## 命令

```sh
python3 Main/sim/mutation/run_mutation.py --list             # 清单
python3 Main/sim/mutation/run_mutation.py --audit            # 规则唯一性、规格出处、见红位置齐全
python3 Main/sim/mutation/run_mutation.py --run              # 全量，写新的运行目录
python3 Main/sim/mutation/run_mutation.py --run --ids blend.wrong_eq
python3 Main/sim/mutation/run_mutation.py --run --autofill-evidence   # 把见红标签写回清单（仍需人工复核）
python3 Main/sim/mutation/verify_migration.py --base-rev <迁移前的提交>
```

运行目录默认 `Main/sim/build/check-runs/mutation-<时间>/`（已在忽略范围）：
`shadow/` 是影子树（`Main/` 整棵复制，去掉 `build`/`logs`/位流），`baseline/` 与
`logs/<变异>/` 是归档的原始输出，`mutation-report.json/md` 是结果。

## 清单的字段

```yaml
- id: bin.bin_clip            # <模块>.<改错什么>
  target: Main/rtl/engine/frontend/engine_bin.v
  macro: INJ_BIN_CLIP         # 只用于追溯原来的 ifdef 开关
  defect: 表面谓词             # 缺陷模型
  spec: {doc: Main/doc/赛题二总体设计.md, section: 3.4 命令与软件接口}
  rules:                      # 同一宏多处生效时是一组，跑器一次全施加
    - {find: "...", replace: "..."}
  runs:
    - cmd: python3 Main/tools/check/check_f5_bin.py --models --bin --frame --protocols --lease
      log: Main/sim/engine/frontend/logs        # 文件或目录；目录只读本轮新写的 *.log
      evidence: MEM_MISMATCH                    # 检查器标签级正则
```

`rules[].find` 必须唯一匹配，上下文不够就往上扩行；`replace` 可以为空（整块删除，
`#ifndef` 类变异就是这种）。`--audit` 会在规则失效或规格出处找不到时直接失败。

## 判定

| 状态 | 含义 | 算不算「判据有效」 |
| --- | --- | --- |
| `KILLED` | 变异体退出非零，且见红位置匹配 `runs[].evidence` | 算 |
| `KILLED-WEAK` | 红了，但见红位置对不上或只靠超时 | 不算，记缺口 |
| `SURVIVED` | 变异体退出 0，判据没抓到 | 不算，记缺口 |
| `PARTIAL` | 登记多条命令，只有部分抓到 | 算部分，记缺口 |
| `INVALID` | 规则不唯一、编译失败、或同轮基线不通过 | 整轮失败（退出码非零） |

基线是同一条命令、同一棵树、不施加规则，要求退出 0。所以套件的 `--case` 模式必须
「该用例不是 PASS 就退出非零」，这是跑器能从退出码判定的前提。

## 套件侧的两个约定

1. **`--case <名字>`**：只构建并运行那一个用例、只打印 `RESULT:`，由跑器判定；
   不加时套件行为不变，按原来的用例表判 PASS/FAIL。
2. **不留注错开关**：变异体由跑器施加，套件里不再有 `-DINJ_*`，也不要有「知道有变异」
   的分支（`INJ_VAL`/`ANY_INJECT` 一类）。

Python 检查没有统一入口，清单的 `runs[].cmd` 直接写整条命令；跑器在影子树根执行它，
脚本从 `__file__` 推出的仓根因此落在影子树上，读到的就是被改过的那份源码。

## 新增一条变异

1. 想清楚它对应哪条规格、模拟哪类真实错误、哪个检查器应该抓到、为什么抓得到。
2. 在 `mutants.yaml` 写 `rules`（可以用 `migrate_ifdef.py --from-rev` 从历史里的
   `ifdef` 生成，也可以手写），`runs[].evidence` 先留空。
3. `--audit` 全绿后 `--run --ids <id> --autofill-evidence`，看它红在哪一行，
   把 evidence 改成检查器标签级正则，再跑一次确认 `KILLED`。

## 还没做

- `run_mutation.py` 尚未注册成 `validate.py` 的检查项，当前是独立命令 + 本轮证据。
- 7 个测试台里的 `INJ_VAL`/`ANY_INJECT` 死代码未清（宏已不存在，恒为假）。
- 整机综合未对账；`engine_regs` / `engine_pb` 两个零变异模块待补。
