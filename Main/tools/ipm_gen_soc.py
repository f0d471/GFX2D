#!/usr/bin/env python3
"""用 Efinity 官方的 IP Manager Python API 重新生成 Sapphire SoC IP。

## 为什么不用 gen_soc.py

`gen_soc.py` 直接调用生成器 `soc_gen.py`，它只产出 `ip/soc/source/` 下的网表与片上 RAM 初始文件。
工程真正引用的 `ip/soc/soc.v` 是 IP Manager 自己做的：按 `ip_component.xml` 里各端口的
`isPresent` 条件裁出包装模块 `soc`，再把网表的模块名加上唯一后缀拼在后面。
软件包（本仓的 `Main/sdk/`）也不是 `soc_gen.py` 出的，是另一个脚本 `embedded_sw/sw_script.py`。
只动构型、不增减端口时（E1）只换网表还能用；这一次要开 GPIO1、开中断、关 `axiA`，
端口集合变了，必须走完整的 IP Manager 流程。

Efinity 提供了这个流程的 Python 入口 `ipm_api_service.design.IPMDesignAPI`，
官方示例在 `<EFINITY>/project/example_scripts/ipm/example_fifo.py`。本脚本照它的用法：
`create_ip` 取当前安装里最新版本的 efx_soc，把 `settings.json` 里的用户参数逐项 `config_ip`，
`validate_ip` 校验，再 `generate_ip`。

## 运行环境

必须在 Efinity 的 `bin/setup.bat` 设好的环境里用它自带的 python 运行，
否则 `devicedb` 的 DLL 加载失败（`OSError ... libdevicedb_wrapper.dll: error 0x7e`）：

    cmd /c "call <EFINITY>\\bin\\setup.bat && python Main\\tools\\ipm_gen_soc.py --project Main\\fpga\\gfx2d_ti60 ..."

## 用法

    --project   Efinity 工程目录
    --override  覆盖 settings.json 里的参数，形如 GPIO1=1'b1，可给多个
    --outputs   要生成的文件集，默认 generator source script embedded_sw
                （不生成示例工程与仿真文件集，它们会往工程里写一堆用不到的东西）
    --dry-run   只打印参数对照与将要生成的文件集，不生成

`settings.json` 里的 `HexFile_Path` 必须是展开后的真实路径（先 `localize_paths.py --apply`）。

## 输出

- `<工程>/ip/soc/`：`soc.v` 等源文件、网表、片上 RAM 初始文件、改写后的 `settings.json`；
- `<工程>/embedded_sw/soc/`：软件包。本仓把它同步进 `Main/sdk/`，
  之后按 `sdk/README.md` 的「已登记的修改」逐条恢复。

## 注意

- 生成会覆盖 `ip/soc/`，生成前工作区要干净。
- `settings.json` 里有、而新版本 IP 里没有的参数会被列出来；
  新版本 IP 里有、而 `settings.json` 里没有的用户参数取 IP 的默认值，也会列出来。两份清单都要看。
"""
import argparse
import json
import os
import sys
from pathlib import Path


def fail(msg):
    print("ERROR: %s" % msg, file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True)
    ap.add_argument("--gen-name", default="soc")
    ap.add_argument("--override", nargs="*", default=[])
    ap.add_argument("--outputs", nargs="*",
                    default=["generator", "source", "script", "embedded_sw"])
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    efinity = os.environ.get("EFINITY_HOME")
    if not efinity or "EFXIPM_HOME" not in os.environ:
        return fail("没有 Efinity 环境变量，先 call <EFINITY>\\bin\\setup.bat")
    sys.path.append(os.path.join(os.environ["EFXIPM_HOME"], "bin"))
    from ipm_api_service.design import IPMDesignAPI

    project = Path(args.project).resolve()
    settings_path = project / "ip" / args.gen_name / "settings.json"
    if not settings_path.is_file():
        return fail("找不到 %s" % settings_path)
    with open(settings_path, encoding="utf-8") as f:
        settings = json.load(f)
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

    # 器件取自工程文件
    import xml.etree.ElementTree as ET
    proj_xml = project / (project.name + ".xml")
    root = ET.parse(proj_xml).getroot()
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

    design.config_ip(module_name=args.gen_name, configs=configs)
    ok, result, _ = design.validate_ip(module_name=args.gen_name)
    if not ok:
        print(result.error_tuple_msg if result else "")
        return fail("validate_ip 未通过")
    print("validate_ip 通过")

    if args.dry_run:
        print("--dry-run，不生成")
        return 0

    rc = design.generate_ip(module_name=args.gen_name)
    print("generate_ip -> %s" % rc)
    if str(rc) != "ResultCode.SUCCESS":
        return fail("生成失败")
    print("生成完成。软件包在 %s" % (project / "embedded_sw" / args.gen_name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
