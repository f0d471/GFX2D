# tools：辅助脚本

与硬件、软件构建无关的小工具放这里。

| 文件 | 用途 |
| --- | --- |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 用的 hex 文本（每字节一行、两位大写十六进制、LF 换行），写 Flash 前用 |

## bin2hex.py

```
python3 tools/bin2hex.py <输入.bin> <输出.hex>
```

转换后会把 hex 解码回字节与输入逐字节比较，不一致时退出码为 1。

烧写命令见 `doc/板卡实测记录.md` 第 8 节。Efinity 的 `jtag_bridge` 模式只接受 hex 文本，不接受裸 bin，所以写 Flash 前必须先转换。
