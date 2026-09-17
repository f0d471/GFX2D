#!/usr/bin/env python3
"""从命令行生成 Sapphire SoC IP，不经过 IP Manager 的图形界面。

## 为什么需要它

Efinity 的 IP 生成只有图形界面入口（`efx_ipmgr`），工程里要批量生成几套构型做对比
（例如 `SOC_MODE` 取 Standard 还是 Lite、内存时钟走统一还是独立）时，点界面既慢又不留痕。
这个脚本复现 IP Manager 对生成器的调用，把构型写成命令行参数，可重复、可归档。

真正的生成器是厂商安装目录下的 `ipmgr/ip/efx_soc/efx_soc/generator/soc_gen.py`，
它由 `ipmgr/ip/efx_soc/efx_soc/ipm/ip_component.xml` 里的
`external_script_generator` 声明驱动：

    ${EFINITY_PYTHON} ${IP_LIBRARY_ROOT}/efx_soc/efx_soc/generator/soc_gen.py
      --out_path ${IP_OUTPUT_PATH} --user_project_path ${USER_PROJECT_PATH}
      --gen_name ${IP_GEN_MODULE} --parameters ${IP_USER_PARAMETERS}

`--parameters` 是 `nargs="+"`，每一项形如 `KEY=VALUE`。

## 它替生成器补掉的三件事

这三件都是 `soc_gen.py` 的健壮性问题，不补就生成不了。写成代码而不是写进文档，
是因为每一步都容易漏。全部依据都标了出处，便于升级 Efinity 之后重新核对。

1. **Verilog 字面量。** `settings.json` 里大量值形如 `1'b1`、`32'd1048576`；
   `soc_gen.py` 的 `_format_token` 会 `eval()` 这些值，而它的 `try` 只捕 `ValueError`，
   遇到 Verilog 字面量抛的 `SyntaxError` 会直接冒出来。这里先把字面量解析成十进制。

2. **缺键。** 如果 `settings.json` 被手工编辑过（本项目就编辑过：改 `LDSize`、
   `HexFile_Path` 等），`ip_component.xml` 里 `resolve="immediate"` 的**自动参数**
   会全部丢失——它们在界面上不可编辑，所以保存配置时才有值，手工改过的文件里没有。
   而生成模板会引用它们，缺一个就抛 `ValueError: Unknown format code 'x'`。
   缺的键按 `alignAddress()` 的规则补齐，规则见 _AUTOFILL 的注释。

3. **参数类型。** 模板里 `0x{:06x}` 这类格式要求值是 int。地址类参数在
   `ADDR_Scheme == 1` 时由 `alignAddress()` 转成 int，`0` 时不转；
   本项目是 `0`，所以这里显式给出解析后的十进制值。

## 用法

    <EFINITY_PYTHON> tools/gen_soc.py --project <工程目录> [--override K=V ...]

`--project` 指向 Efinity 工程目录，脚本自己从 `<工程>/ip/soc/settings.json` 读基准参数。
`--override` 按需覆盖，例如：

    --override SOC_MODE=1 DDRCLK_DOMAIN=1 MULDIV_EXT=1 BARREL_SHIFTER=1

`--dry-run` 只打印将要传给生成器的参数与命令行，不实际生成。

## 注意

- **生成会覆盖 `<工程>/ip/soc/` 与软件包。** 生成前先提交或备份，
  生成后按 `sdk/README.md` 的「已登记的修改」逐条恢复。
- `settings.json` 里的 `--base_path` 与 `HexFile_Path` 是绝对路径，
  用 `tools/localize_paths.py --apply` 展开。本脚本读展开后的值。
- 生成器脚本与 IP 定义文件在 Efinity 安装目录下，**本脚本不修改它们**。
  早期排查时曾临时给 `soc_gen.py` 加过打印，已还原；如需诊断见 `--dry-run` 与报错信息。
"""
import argparse
import json
import os
import re
import subprocess
import sys

# Verilog 字面量：<宽度>'<进制><数字>，进制 b/h/d/o
_LITERAL = re.compile(r"\b(\d+)'([bhd o])([0-9a-fA-F]+)\b".replace(" ", ""))
_BASES = {"b": 2, "h": 16, "d": 10, "o": 8}

# ip_component.xml 里 resolve="immediate" 的自动参数，以及它们从哪个键、按什么掩码推出。
# 规则来自 soc_gen.py 的 alignAddress()：
#   外设     X_Addr = X_M_Addr & 0xffffff000
#   APB 从机 APBSlaveN_Addr = APBSlaveN_M_Addr & 掩码（掩码由 APBSlave0_Size 决定）
#   Base_AXIS / Base_IO = Base_M_* & 0xff000000
#   Base_IM 在 settings.json 里没有对应键，取 ip_component.xml 的默认片上 RAM 地址
_IM_BASE_DEFAULT = 0xF9000000


def resolve_literals(text):
    """把字符串里的 Verilog 字面量换成十进制，其余原样保留。"""
    def conv(m):
        return str(int(m.group(3), _BASES[m.group(2)]))
    return _LITERAL.sub(conv, text)


def find_efinity():
    """定位 Efinity 安装目录。

    仓库不写绝对路径（见仓库根 README），所以这里按两条与环境有关的路子找：
    `EFINITY_HOME` 环境变量，或者 PATH 里能搜到 `efx_run.bat`（Efinity 安装时
    会把 `<EFINITY>\\bin` 加进 PATH，`doc/开发环境.md` 第 5 节就是用它编译）。
    都找不到就要求显式给 `--efinity`。
    """
    for key in ("EFINITY_HOME", "EFINITY_ROOT"):
        v = os.environ.get(key)
        if v and os.path.isdir(v):
            return v
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if d and os.path.isfile(os.path.join(d, "efx_run.bat")):
            return os.path.dirname(d)
    return None


def fail(msg):
    print("ERROR: %s" % msg, file=sys.stderr)
    return 1


def apb_mask(size):
    """APBSlaveN_Addr 的掩码，规则取自 alignAddress()。"""
    if 65536 <= size < 1048576:
        return 0xFFFF0000
    if size == 1048576:
        return 0xFFF00000
    return 0xFFFFF000


def autofill(conf):
    """补齐模板引用、而 settings.json 里没有的自动参数。返回补了哪些键。"""
    added = []
    apb_size = int(resolve_literals(str(conf.get("APBSlave0_Size", "65536")).strip('"')))
    mask = apb_mask(apb_size)

    for key in list(conf.keys()):
        if not key.endswith("_M_Addr"):
            continue
        short = key[: -len("_M_Addr")] + "_Addr"
        if short in conf:
            continue
        m = mask if key.startswith("APBSlave") else 0xFFFFFF000
        conf[short] = str(int(resolve_literals(str(conf[key]).strip('"'))) & m)
        added.append(short)

    for full, short, m in (("Base_M_AXIS", "Base_AXIS", 0xFF000000),
                           ("Base_M_IO", "Base_IO", 0xFF000000)):
        if full in conf and short not in conf:
            conf[short] = str(int(resolve_literals(str(conf[full]).strip('"'))) & m)
            added.append(short)

    if "Base_IM" not in conf:
        conf["Base_IM"] = str(_IM_BASE_DEFAULT)
        added.append("Base_IM")

    # 关闭的 APB 从机（APBSlaveN == 0）其模板条目会被 enable 条件跳过，
    # 只有 APBSlave0 在用，所以其余几个的 Size 给 0 不影响结果。
    for n in range(1, 5):
        key = "APBSlave%d_Size" % n
        if key not in conf:
            conf[key] = "0"
            added.append(key)

    return added


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True,
                    help="Efinity 工程目录，例如 Main/fpga/gfx2d_ti60")
    ap.add_argument("--settings", default=None,
                    help="配置文件名，默认 <project>/ip/soc/settings.json")
    ap.add_argument("--gen-name", default="soc", help="IP 实例名，默认 soc")
    ap.add_argument("--override", nargs="*", default=[],
                    help="覆盖参数，形如 SOC_MODE=1，可给多个")
    ap.add_argument("--efinity", default=None, help="Efinity 安装目录")
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印参数与命令行，不生成")
    args = ap.parse_args()

    project = os.path.abspath(args.project)
    settings = args.settings or os.path.join(project, "ip", args.gen_name, "settings.json")
    if not os.path.isfile(settings):
        return fail("找不到配置文件 %s" % settings)

    efinity = args.efinity or find_efinity()
    if not efinity:
        return fail("找不到 Efinity 安装目录，用 --efinity 指定，或设 EFINITY_HOME")

    soc_gen = os.path.join(efinity, "ipm", "ip", "efx_soc", "efx_soc",
                           "generator", "soc_gen.py")
    python = os.path.join(efinity, "python311", "bin", "python.exe")
    for p in (soc_gen, python):
        if not os.path.isfile(p):
            return fail("缺文件 %s（Efinity 版本或目录结构不对）" % p)

    with open(settings, "r", encoding="utf-8") as f:
        conf = dict(json.load(f)["conf"])

    overrides = []
    for item in args.override:
        if "=" not in item:
            return fail("覆盖项要写成 KEY=VALUE，收到 %r" % item)
        k, _, v = item.partition("=")
        k, v = k.strip(), v.strip()
        if k not in conf:
            return fail("覆盖项 %s 不在 %s 里；键名写错会静默无效，所以这里直接报错"
                        % (k, os.path.basename(settings)))
        conf[k] = resolve_literals(v)
        overrides.append("%s=%s" % (k, conf[k]))

    added = autofill(conf)
    params = ["%s=%s" % (k, resolve_literals(str(v))) for k, v in conf.items()]

    cmd = [python, soc_gen,
           "--out_path", os.path.join(project, "ip"),
           "--user_project_path", project,
           "--gen_name", args.gen_name,
           "--parameters"] + params

    print("工程      : %s" % project)
    print("配置      : %s" % settings)
    print("Efinity   : %s" % efinity)
    if overrides:
        print("覆盖      : %s" % " ".join(overrides))
    else:
        print("覆盖      : （无，按 settings.json 原样生成）")
    if added:
        print("补齐缺键  : %s" % " ".join(sorted(added)))
    print("参数总量  : %d 项" % len(params))

    if args.dry_run:
        print("\n--dry-run，命令行如下（不执行）：")
        print(" ".join(cmd[:4]), "--parameters", "<%d 项>" % len(params))
        for p in params:
            print("   ", p)
        return 0

    env = dict(os.environ)
    env["PYTHONHOME"] = os.path.join(efinity, "python311")
    env["EFINITY_HOME"] = efinity
    print("\n开始生成……（会覆盖 %s/ip/%s 与软件包，生成前请确认已提交或备份）"
          % (project, args.gen_name))
    rc = subprocess.call(cmd, env=env)
    if rc != 0:
        return fail("生成器退出码 %d" % rc)
    print("生成完成。下一步：按 sdk/README.md 的「已登记的修改」逐条恢复。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
