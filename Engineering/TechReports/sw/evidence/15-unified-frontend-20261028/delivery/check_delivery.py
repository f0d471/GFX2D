"""Read-only delivery checks: untouched user files, preserved guard, doc links."""
from pathlib import Path
import ast
import hashlib
import json
import re
import subprocess
from urllib.parse import unquote

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
before=json.loads((HERE/'unrelated-before.json').read_text(encoding='utf-8'))
changed=[name for name,sha in before.items() if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=sha]
report_updates = {
    'Deliverables/Report/gfx2d技术报告.docx',
    'Deliverables/Report/gfx2d技术报告.pdf',
    'Deliverables/Report/parts/00-封面摘要.md',
    'Deliverables/Report/parts/01-作品概述.md',
    'Deliverables/Report/parts/04-完成情况.md',
}
unexpected = [name for name in changed if name not in report_updates]
external = [dict(path=name, before_sha256=before[name],
                 current_sha256=hashlib.sha256((ROOT/name).read_bytes()).hexdigest(),
                 mtime_ns=(ROOT/name).stat().st_mtime_ns)
            for name in changed if name in report_updates]
def methods(path):
    text=path.read_text(encoding='utf-8-sig')
    panel=next(x for x in ast.parse(text).body if isinstance(x,ast.ClassDef) and x.name=='ControlPanel')
    return {x.name:ast.dump(x,include_attributes=False) for x in panel.body
            if isinstance(x,ast.FunctionDef) and x.name in ('poll','_poll_body')}
guard=methods(HERE/'baseline/Main/tools/board/game_control.py')==methods(ROOT/'Main/tools/board/game_control.py')
names=['Main/README.md','Main/sw/README.md','Main/tools/board/README.md','Main/doc/应用与输入契约.md',
       'Engineering/TechReports/sw/README.md','Engineering/TechReports/sw/13-RTOS移植与游戏机前端.md',
       'Engineering/TechReports/sw/14-架构收敛与OSAL.md','Engineering/TechReports/sw/15-统一应用框架与软件可靠性收敛.md',
       'Engineering/Todo/README.md','Engineering/Todo/sw/软件架构与应用机制收敛.md',
       'Engineering/Todo/sw/启动序列去重.md','Engineering/Todo/sw/RTOS移植与游戏机前端.md']
missing=[];count=0
for name in names:
    path=ROOT/name;text=path.read_text(encoding='utf-8-sig')
    for match in re.finditer(r'!?\[[^\]]*\]\(([^)]+)\)',text):
        target=match[1].strip().strip('<>')
        if target.startswith(('http:','https:','mailto:','#')):continue
        target=unquote(target.split('#',1)[0]);count+=1
        if not (path.parent/target).exists():missing.append(dict(source=name,target=target))
result=dict(unrelated_changed=changed,unexpected_unrelated_changes=unexpected,
            separately_observed_report_updates=external,
            report_update_note='These report files were not edited by this software task; preserve their current contents. This is not a byte-unchanged claim for the whole report directory.',
            user_poll_guard_preserved=guard,local_links_checked=count,missing_links=missing)
(HERE/'delivery-checks.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(result,ensure_ascii=False))
raise SystemExit(bool(unexpected or not guard or missing))
