# 引擎可选机制修复证据

档位与实际范围由各目录的 plan.json/result.json 给出。当前源码的正式结果与
参数研究、失败现场分开列出；收到 done、读到 CAPS 或旧日志通过均不替代功能判据。

| 目录 | 范围与边界 |
| --- | --- |
| [blit-isolation/](blit-isolation/) | 最初 quick 隔离对照：B0 FILL 正常、B0/B1 BLIT 超时、B2 与补常量的 B0 正常；不代表完整验收 |
| [final-rtl/](final-rtl/) | 最后两处共享控制修复后的 acceptance：160 合法构型、六个正式变异、模块及 16 项整帧检查 |
| [final-control/](final-control/) | 同版本控制层 acceptance：159 向量、定向用例和两个注错构建 |
| [b0-adoption/](b0-adoption/) | 最新主工程 B0 四步、基本功能板测、生产 HW/DDR 游戏 240 帧；不代表逐帧完整关卡回放 |
| [pb-no-strip-final-synthesis/](pb-no-strip-final-synthesis/) | 最新 PB 开启、STRIP 关闭补充构型的四步与全时钟检查 |
| [final-seven/](final-seven/) | 最新源码七档重新四步、全时钟/网表与同一 BIN 的 93 例实际板测 |
| [pb-no-strip-final-board/](pb-no-strip-final-board/) | 最新补充构型同一 BIN 14 例，含实例/PB 与独立 STRIP 拒绝 |
| [final-b5-consumers/](final-b5-consumers/) | 最新 B5 隔离源树的实例 144 组、分桶 10000 张及协议/变异/生命周期 |
| [final-board-restore/](final-board-restore/) | 七档结束后恢复主工程 B0，再次完成生产游戏 240 帧 |
| [config-model/](config-model/) | 512 个模型输入；160 合法，352 非法逐个在产生文件前被拒绝 |
| [pb-mutations/](pb-mutations/) | 恢复 PB 条带启动选择器丢失、共享寄存器错误裁剪，均被真实消费者检出 |
| [configuration/](configuration/) | 前期 160 构型与四个变异；尚未含后端切换和软复位新增判据 |
| [b5-consumers/](b5-consumers/) | 前期 B5 实例 144 组、分桶 10000 张与相关协议；版本边界见 run.json |
| [pb-no-strip-board/](pb-no-strip-board/) | 前期补充构型 13 例板测及八个解析负例，未含后加的实例/STRIP 切换 |
| [pb-no-strip-synthesis/](pb-no-strip-synthesis/) | 前期补充构型综合取样 |
| [final-synthesis/](final-synthesis/) | 中间轮编译输入复用与复制校验，早于后端切换/裁剪框修复；不是最新七档源码结果 |
| [b5-speed2/](b5-speed2/) | speed2 参数研究的通过对象，60310 XLR；早于最后两处 RTL 修复 |
| [speed-initial/](speed-initial/) | speed 的 B5 60838 超容量两次及外层 1800 秒预算失败现场；历史快照 |
| [area-experiment/](area-experiment/) | area 模式 62502 超容量；历史失败快照 |
| [area2-experiment/](area2-experiment/) | area2 模式 60838 超容量；历史失败快照 |
| [workflow-initial/](workflow-initial/) | 过期板测样本缺能力配对标记，流程测试拒绝；历史失败快照 |
| [backend-transition-failure/](backend-transition-failure/) | B5 的 PB 完成后 STRIP 返回成功却漏绘整面；历史失败快照，保留未完成的七档板测范围 |
| [game-capability-failure/](game-capability-failure/) | B0 生产游戏无 TEX 被误记装载失败；历史失败快照 |
| [reset-clip-failure/](reset-clip-failure/) | B0 软复位仍保留裁剪框，控制层向量失败；历史失败快照 |
| [launcher-review/](launcher-review/) | 收口脚本入口 11 项：环境缺失、输出越界、链接拒绝、七档参数与失败传递；模拟编译器，不重跑综合或板测 |

完整身份只保存在各轮 run.json 与 archive-files.json。后者校验归档复制的原始字节，
不代表算法、板测或综合功能通过。原始 Python 实验快照以 .py.log 保存，
同盘改名不改变内容，继续复用已登记摘要；原始日志保持可追溯。
位流和构建中间件留在对应忽略运行目录，章节只引用范围、结论与本索引。
