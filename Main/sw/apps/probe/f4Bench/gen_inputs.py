#!/usr/bin/env python3
"""把 F1 冻结的固定输入转成 f4Bench 的 C 数组。

用法（Efinity Python）：在 Main/sim/engine/frontend 下
    <EFINITY>/python311/bin/python.exe ../../../sw/apps/probe/f4Bench/gen_inputs.py \
        inputs/scene00_mode0.bin inputs/scene04_mode0.bin inputs/scene14_mode0.bin
输出 f4_scenes.c/f4_scenes.h 到应用 src/ 目录；文件头部记录来源与字节数，
源输入本身不入库到应用，仍是 sim/engine/frontend/inputs 的登记物。
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    scenes = []
    for arg in argv[1:]:
        p = Path(arg)
        data = p.read_bytes()
        if len(data) < 196 or (len(data) - 196) % 32 != 0:
            print("bad input:", p)
            return 2
        scenes.append((p.stem, data))
    hdr = ["/* 由 gen_inputs.py 生成；来源 sim/engine/frontend/inputs，勿手改。 */",
           "#ifndef F4_SCENES_H", "#define F4_SCENES_H", "",
           "#include <stdint.h>", "",
           "typedef struct {",
           "    const char *name;",
           "    const uint8_t *data;",
           "    uint32_t bytes;",
           "} f4_scene;", ""]
    c = ["/* 由 gen_inputs.py 生成；来源 sim/engine/frontend/inputs，勿手改。 */",
         '#include "f4_scenes.h"', ""]
    for name, data in scenes:
        hdr.append("extern const uint8_t f4_%s[%u];" % (name, len(data)))
        c.append("const uint8_t f4_%s[%u] = {" % (name, len(data)))
        for off in range(0, len(data), 16):
            chunk = data[off:off + 16]
            c.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")
        c.append("};")
        c.append("")
    hdr.append("extern const f4_scene f4_scene_table[%u];" % len(scenes))
    hdr.append("extern const uint32_t f4_scene_count;")
    hdr.append("")
    hdr.append("#endif /* F4_SCENES_H */")
    rows = ["    { \"%s\", f4_%s, %u }," % (n, n, len(d)) for n, d in scenes]
    c.append("const f4_scene f4_scene_table[%u] = {" % len(scenes))
    c += rows
    c.append("};")
    c.append("")
    c.append("const uint32_t f4_scene_count = %u;" % len(scenes))
    c.append("")
    (SRC / "f4_scenes.h").write_text("\n".join(hdr), encoding="utf-8", newline="\n")
    (SRC / "f4_scenes.c").write_text("\n".join(c), encoding="utf-8", newline="\n")
    print("wrote %d scenes, total %d bytes" % (
        len(scenes), sum(len(d) for _, d in scenes)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
