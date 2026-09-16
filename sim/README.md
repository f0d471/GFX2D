# sim：仿真

用 WSL 里的 iverilog 跑，不依赖 Efinity。每个子目录一个 `run.sh`，进去直接运行：

```
wsl -e bash -c 'cd /mnt/e/Workspace/Efinix/Main && bash sim/<目录>/run.sh'
```

## 目录

| 目录 | 被测模块 | 用例 |
| --- | --- | --- |
| `hdmi/` | `rtl/hdmi/hdmi_tx.v` 整条通路 | 正常 + 2 个注错 |
| `apb_probe/` | `rtl/bus/apb_probe.v` | 正常 + 2 个注错 |

## 规则

每个 `run.sh` 除了正常用例，还必须带注错用例：把一处改错，对应的判据必须报 FAIL。三个用例都符合预期才输出 `SIM OK`，否则退出码非零。只跑正常用例无法说明判据真的在检查东西。

| 用例 | 注错内容 | 期望 |
| --- | --- | --- |
| `hdmi/inject_hbp` | 行后沿少 1 个像素 | FAIL |
| `hdmi/inject_noinv` | 不撤销 P/N 取反 | FAIL |
| `apb_probe/inject_addr_trunc` | 只把地址低 4 位送进被测模块 | FAIL |
| `apb_probe/inject_data_trunc` | 只把写数据最低位送进被测模块 | FAIL |

后两个注错对应的是顶层曾经真实存在的 bug：APB 信号在顶层没有声明，被当成 1 位线，32 位地址和数据都被截断。

## 范围

DDR3 控制器、SoC、`axi_interconnect_beta` 都是加密 RTL，开源工具读不了，所以整机仿不了。能仿的只有明文模块。引擎开发阶段要自己写 AXI 从机模型，并且必须带随机反压和可配的响应延迟，测试台里的从机比真从机强等于没验。

产物在各子目录的 `build/` 下，不进版本库。
