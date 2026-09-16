# tools：辅助脚本

与硬件、软件构建无关的小工具放这里。

| 文件 | 用途 |
| --- | --- |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 用的 hex 文本（每字节一行、两位大写十六进制、LF 换行），写 Flash 前用 |
| `localize_paths.py` | 展开、还原与检查仓库里的绝对路径 |

两个脚本都只用标准库。本机如果没有 Windows 版 python，用 `wsl -e python3 <脚本>` 调用。

## localize_paths.py

有五个文件由厂商工具生成，里面必须写绝对路径：`fpga/gfx2d_ti60/ip/soc/settings.json`
与 gpioDemo 的 `.cproject`、三个 `.launch`。仓库里存的是占位符形式。

```
python3 tools/localize_paths.py --apply      占位符 -> 本机路径，克隆之后跑一次
python3 tools/localize_paths.py --restore    本机路径 -> 占位符，提交之前跑一次
python3 tools/localize_paths.py --check      扫描整个仓库，发现机器专属路径就以非零退出码结束
```

`--check` 找的是盘符路径、`/home/<用户>/` 和 `/mnt/<盘符>/` 三类写法。
厂商代码与 IP 生成物不在扫描范围内，它们带的是生成时那台机器的工具安装位置。
唯一允许出现实际路径的文档是 `doc/开发环境.md` 第 11 节，脚本里已登记豁免。

## bin2hex.py

```
python3 tools/bin2hex.py <输入.bin> <输出.hex>
```

转换后会把 hex 解码回字节与输入逐字节比较，不一致时退出码为 1。

烧写命令见 `doc/开发环境.md` 第 8 节。Efinity 的 `jtag_bridge` 模式只接受 hex 文本，不接受裸 bin，所以写 Flash 前必须先转换。
