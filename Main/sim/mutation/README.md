# 变异测试活动

生产 RTL 里不放注错分支。变异体写在本目录的 [`mutants.yaml`](mutants.yaml)，
跑器在隔离影子树里施加一条，再用 `Main/sim/` 下原有的测试台跑，按登记的见红位置判定。
它回答的问题只有一个：**判据到底有没有在检查东西**。

当前进度：`engine_blend` 与 `engine_pix` 已迁（10 条），其余模块仍是 RTL 里的
`` `ifdef INJ_* ``（见 `Main/doc/开发环境.md` 0.20 节）。

## 文件

| 文件 | 作用 |
| --- | --- |
| `mutants.yaml` | 唯一真相：一条变异一行，含缺陷模型、规格出处、字面替换规则、应发现它的用例与见红位置 |
| `run_mutation.py` | 跑器：建影子树、应用一条规则、跑基线+变异体、分类、写报告；`--audit` 只校验不跑仿真 |
| `verify_migration.py` | 迁移等价性证明：正向（迁移前后展开逐行相同；只有注释变时退到代码相同）、反演（规则应用回去后与原来 `-D<宏>` 的展开词法相同） |
| `migrate_ifdef.py` | 迁移期一次性工具：把 `` `ifdef INJ_* `` 块抽成规则并删除变异分支；最后一个模块迁完删除 |

## 命令

```sh
python3 Main/sim/mutation/run_mutation.py --list          # 清单
python3 Main/sim/mutation/run_mutation.py --audit         # 规则唯一性、规格出处、见红位置齐全
python3 Main/sim/mutation/run_mutation.py --run           # 全量，写新的运行目录
python3 Main/sim/mutation/run_mutation.py --run --ids blend.wrong_eq
python3 Main/sim/mutation/verify_migration.py --base-rev <迁移前的提交>
```

运行目录默认 `Main/sim/build/check-runs/mutation-<时间>/`（已在忽略范围）。里面：
`shadow/` 是影子树（`Main/rtl`、`Main/tools/check`、`Main/sw/lib` 与本次涉及的套件，
相对布局不变，所以套件里的 `../../../rtl` 照旧成立），`baseline/` 与 `runs/<变异>/`
是归档的用例日志，`mutation-report.json` / `mutation-report.md` 是结果。

## 状态

| 状态 | 含义 | 算不算「判据有效」 |
| --- | --- | --- |
| `KILLED` | 变异体 FAIL，且见红位置匹配该用例登记的 `evidence` 正则 | 算 |
| `KILLED-WEAK` | FAIL 了，但红在别处，或只靠超时 | 不算，记缺口 |
| `SURVIVED` | 变异体 PASS | 不算，记缺口 |
| `PARTIAL` | 登记了多个用例，只有部分抓到 | 算部分，记缺口 |
| `INVALID` | 规则不唯一、编译失败、或同树基线不 PASS | 整轮失败（退出码非零） |

分母是「全部 − 有 `proof` 的 EQUIVALENT」。等价变异必须给出已跑过的等价论证
（例如混合核的全定义域穷举），不能靠「看起来等价」处理。

## 套件侧的唯一约定

套件的 `run.sh` 支持 `--case <用例名>`：只构建并运行那一个用例、只打印 `RESULT:`，
**不自己判定**。判定归 `mutants.yaml`（每条用例一条正则）。`--case` 不加时行为不变。
不跑单个用例时，`run.sh` 仍按原来的用例表判 PASS/FAIL。

跑器从套件目录的 `build/<用例>.log` 取证据，所以 `--case` 必须走套件原有的构建函数，
不要另写一条编译命令，否则源列表会有第二份真相。

## 迁移一个模块的三步

1. `migrate_ifdef.py --file <rtl> --yaml <out> --write`：抽出规则、删掉变异分支。
2. 规则合并进 `mutants.yaml`，补 `defect`、`spec`、`runs[].evidence`（先跑一次，从
   `runs/<变异>/<用例>.log` 里挑真正判红的那一行，正则写到标签级，不要写死数值）。
3. `verify_migration.py --base-rev <迁移前提交>`：正向逐行相同、反演词法相同，才算迁移
   没改生产逻辑。然后 `--audit`、`--run`，全部 `KILLED` 才收工。

## 还没做

- 剩余 12 个文件、60 条变异；其中 5 个套件由 Python 表驱动（band/stripctl/instances/bin/g4），
  要先改成吃影子树。
- `run_mutation.py` 还没注册成 `validate.py` 的检查项，当前结果是本轮证据，不进档位。
- 迁移后仍有 7 个测试台持有 `INJ_VAL`/`ANY_INJECT` 一类「知道有变异」的开关（mover、geom、
  tex 等），这些模块迁移时必须一起删掉，否则它们会把变异体的行为当作正常流程的一部分。
