#!/usr/bin/env python3
"""用 Efinity 官方的 IP Manager Python API 重新生成 Sapphire SoC IP。

## 为什么走 IP Manager 的 API

工程真正引用的 `ip/soc/soc.v` 是 IP Manager 自己做的：按 `ip_component.xml` 里各端口的
`isPresent` 条件裁出包装模块 `soc`，再把网表的模块名加上唯一后缀拼在后面。
软件包（本仓的 `Main/sdk/`）由另一个脚本 `embedded_sw/sw_script.py` 出。
只调生成器 `soc_gen.py` 得到的只有网表，端口一增减就不够用，所以走完整的 IP Manager 流程。

Efinity 提供了这个流程的 Python 入口 `ipm_api_service.design.IPMDesignAPI`，
官方示例在 `<EFINITY>/project/example_scripts/ipm/example_fifo.py`。本脚本照它的用法：
`create_ip` 取当前安装里最新版本的 efx_soc，把 `settings.json` 里的用户参数逐项 `config_ip`，
`validate_ip` 校验，再 `generate_ip`。

## 运行环境

必须在 Efinity 的 `bin/setup.bat` 设好的环境里用它自带的 python 运行，
否则 `devicedb` 的 DLL 加载失败（`OSError ... libdevicedb_wrapper.dll: error 0x7e`）：

    cmd /c "call <EFINITY>\\bin\\setup.bat && python Main\\tools\\soc\\ipm_gen_soc.py --project Main\\fpga\\gfx2d_ti60 ..."

工程目录要与 `HexFile_Path` 指向的 bootloader hex 在同一个盘，否则生成器的 `relpath` 跨盘报错。

## 用法

    --project   Efinity 工程目录（主线工程，或仓库外的复制工程）
    --override  覆盖 settings.json 里的参数，形如 DCacheSize=8192，可给多个；键名不存在直接报错
    --outputs   要生成的文件集，默认 generator source script embedded_sw
                （不生成示例工程与仿真文件集，它们会往工程里写一堆用不到的东西）
    --sync-sdk  生成后把软件包整体复制到这个目录（主线给 Main/sdk），再删掉 embedded_sw/
    --dry-run   只打印参数对照与将要生成的文件集，不生成

`settings.json` 里的 `HexFile_Path` 必须是展开后的真实路径（先 `localize_paths.py --apply`）。

## 生成之后本脚本替你做的三件事

1. API 会把 `settings.json` 里 `output.external_source_source` 等文件清单写成空列表
   （图形界面生成时记着 `soc.v` 等源文件）。生成前的清单非空、生成后变空的，按原样补回。
2. `ip/<实例>/` 下新冒出来的示例目录（`T120F324_devkit/`、`Ti60F225_devkit/`、`Testbench/`）删掉，不入库。
3. 给了 `--sync-sdk` 时同步软件包。复制是覆盖式的，目标目录里生成器不产的文件（`README.md`、
   512KB bootloader 等）不受影响；`sdk/README.md` 登记的修改会被冲掉，要逐条恢复。

## 注意

- 生成会覆盖 `ip/<实例>/`，生成前工作区要干净。
- `settings.json` 里有、而新版本 IP 里没有的参数会被列出来；
  新版本 IP 里有、而 `settings.json` 里没有的用户参数取 IP 的默认值，也会列出来。两份清单都要看。
"""
import argparse
import json
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EXAMPLE_DIRS = ("T120F324_devkit", "Ti60F225_devkit", "Testbench")


def fail(msg):
    print("ERROR: %s" % msg, file=sys.stderr)
    return 1


def list_block(text, key):
    """settings.json 文本里 "key": [...] 这一段的匹配。按文本替换而不是重写 JSON，以保留原格式与行尾。"""
    return re.search(r'"%s":\s*\[[^\]]*\]' % re.escape(key), text)


def restore_empty_lists(path, before_text):
    """生成前非空、生成后变成空列表的 output.* 清单，按生成前的原文补回。返回补回的键。"""
    before = json.loads(before_text).get("output", {})
    after_text = path.read_bytes().decode("utf-8")
    after = json.loads(after_text).get("output", {})
    fixed = []
    for key, old in before.items():
        if old and after.get(key) == []:
            new_m, old_m = list_block(after_text, key), list_block(before_text, key)
            after_text = after_text[:new_m.start()] + old_m.group(0) + after_text[new_m.end():]
            fixed.append(key)
    if fixed:
        merged = json.loads(after_text)["output"]
        if any(merged[k] != before[k] for k in fixed):
            raise RuntimeError("补回文件清单后 settings.json 解析结果不对")
        path.write_bytes(after_text.encode("utf-8"))
    return fixed


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True)
    ap.add_argument("--gen-name", default="soc")
    ap.add_argument("--override", nargs="*", default=[])
    ap.add_argument("--outputs", nargs="*",
                    default=["generator", "source", "script", "embedded_sw"])
    ap.add_argument("--sync-sdk", default=None, help="生成后把软件包复制到这个目录")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not os.environ.get("EFINITY_HOME") or "EFXIPM_HOME" not in os.environ:
        return fail("没有 Efinity 环境变量，先 call <EFINITY>\\bin\\setup.bat")
    sys.path.append(os.path.join(os.environ["EFXIPM_HOME"], "bin"))
    from ipm_api_service.design import IPMDesignAPI

    project = Path(args.project).resolve()
    ip_dir = project / "ip" / args.gen_name
    settings_path = ip_dir / "settings.json"
    if not settings_path.is_file():
        return fail("找不到 %s" % settings_path)
    settings_text = settings_path.read_bytes().decode("utf-8")
    settings = json.loads(settings_text)
    conf = dict(settings["conf"])
    old_ver = settings["args"][-1].get("version", "?")

    for item in args.override:
        k, sep, v = item.partition("=")
        if not sep:
            return fail("覆盖项要写成 KEY=VALUE，收到 %r" % item)
        if k not in conf:
            return fail("覆盖项 %s 不在 settings.json 里，键名写错会静默无效" % k)
        conf[k] = v

    if "__REPO_ROOT" in conf.get("HexFile_Path", ""):
        return fail("HexFile_Path 还是占位符，先运行 localize_paths.py --apply")
    sdk_dst = Path(args.sync_sdk).resolve() if args.sync_sdk else None
    if sdk_dst and not sdk_dst.is_dir():
        return fail("--sync-sdk 的目录不存在：%s" % sdk_dst)

    # 器件取自工程文件
    root = ET.parse(project / (project.name + ".xml")).getroot()
    ns = {"efx": "http://www.efinixinc.com/enf_proj"}
    family = root.find("efx:device_info/efx:family", ns).get("name")
    device = root.find("efx:device_info/efx:device", ns).get("name")

    design = IPMDesignAPI(device_name=device, family_name=family,
                          project_path=str(project), is_verbose=False)
    created = design.create_ip(module_name=args.gen_name, vendor="efinixinc.com",
                               library="soc", name="efx_soc")
    if not created:
        return fail("create_ip 失败")
    vlnv = created[0]
    ip = design._IPMDesignAPI__ips[args.gen_name]

    params = {p.name: p for p in ip["params"]}
    user_keys = {n for n, p in params.items() if p.resolve == "user"}
    unknown = sorted(k for k in conf if k not in params)
    auto_in_settings = sorted(k for k in conf if k in params and k not in user_keys)
    defaults_used = sorted(k for k in user_keys if k not in conf)
    configs = {k: v for k, v in conf.items() if k in user_keys}

    print("工程       : %s（%s %s）" % (project, family, device))
    print("IP 版本    : %s -> %s" % (old_ver, vlnv.version))
    print("覆盖       : %s" % (" ".join(args.override) or "（无）"))
    print("设置的用户参数 %d 项" % len(configs))
    print("settings 里有、新版本里没有（忽略）：%s" % (" ".join(unknown) or "无"))
    print("settings 里是自动参数（由 IP 重新推导）：%s" % (" ".join(auto_in_settings) or "无"))
    print("新版本的用户参数取默认值：")
    for k in defaults_used:
        print("    %s = %s" % (k, params[k].value))
    if not defaults_used:
        print("    无")

    wanted = set(args.outputs)
    outs = [o for o in ip["outputs"] if o.name in wanted]
    missing = wanted - {o.name for o in outs}
    if missing:
        return fail("没有这些文件集：%s；可选的是 %s"
                    % (" ".join(sorted(missing)), " ".join(o.name for o in ip["outputs"])))
    ip["outputs"] = outs
    print("文件集     : %s" % " ".join(o.name for o in outs))
    print("同步软件包 : %s" % (sdk_dst or "不同步（软件包留在 embedded_sw/）"))

    design.config_ip(module_name=args.gen_name, configs=configs)
    ok, result, _ = design.validate_ip(module_name=args.gen_name)
    if not ok:
        print(result.error_tuple_msg if result else "")
        return fail("validate_ip 未通过")
    print("validate_ip 通过")

    if args.dry_run:
        print("--dry-run，不生成")
        return 0

    existed = {p.name for p in ip_dir.iterdir()}
    # 生成器 soc_gen.py 以子进程运行，按 pyrsistent 的 PMap 遍历顺序写 soc_config，
    # 那个顺序随字符串哈希变；Python 默认每个进程随机哈希种子，于是同一份配置每次生成
    # soc_config 的选项顺序、网表里外设的内部编号都不一样（功能等价，但网表整片改动、扰动布局）。
    # 固定种子让同一份配置生成的结果只差 IP_UUID 与 Git hash 两处。
    os.environ["PYTHONHASHSEED"] = "0"
    rc = design.generate_ip(module_name=args.gen_name)
    print("generate_ip -> %s" % rc)
    if str(rc) != "ResultCode.SUCCESS":
        return fail("生成失败")

    fixed = restore_empty_lists(settings_path, settings_text)
    print("补回文件清单：%s" % (" ".join(fixed) or "无需补回"))
    for d in EXAMPLE_DIRS:
        if d not in existed and (ip_dir / d).is_dir():
            shutil.rmtree(ip_dir / d)
            print("删除示例目录 %s" % (ip_dir / d))

    sw_src = project / "embedded_sw" / args.gen_name
    if sdk_dst:
        shutil.copytree(sw_src, sdk_dst, dirs_exist_ok=True)
        shutil.rmtree(project / "embedded_sw")
        print("软件包已复制到 %s，embedded_sw/ 已删除" % sdk_dst)
        print("下一步：按 sdk/README.md「已登记的修改」逐条恢复，"
              "再看 git diff，剩下的每一处都要说得出来源。")
    else:
        print("软件包在 %s" % sw_src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
