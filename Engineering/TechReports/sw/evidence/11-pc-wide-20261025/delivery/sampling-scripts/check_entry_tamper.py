import ast,hashlib,json,shutil,subprocess
from pathlib import Path
folder=Path(__file__).resolve().parent;root=folder.parents[3]
out=folder/'entry-tamper-final';out.mkdir(exist_ok=False);(out/'pc').mkdir()
tree=ast.parse((root/'Main/tools/board/package_game.py').read_text(encoding='utf-8'))
scripts=[n.args[0].value for n in ast.walk(tree) if isinstance(n,ast.Call) and
    isinstance(n.func,ast.Attribute) and n.func.attr=='write_text' and n.args and
    isinstance(n.args[0],ast.Constant) and isinstance(n.args[0].value,str) and
    n.args[0].value.startswith('param([switch]$Fullscreen')]
assert len(scripts)==1
(out/'Run-PC.ps1').write_text(scripts[0],encoding='utf-8-sig')
source=folder/'windows-release/gfx2dGame.exe';target=out/'pc/gfx2dGame.exe'
shutil.copy2(source,target)
expected=hashlib.sha256(target.read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps(dict(files=[dict(path='pc/gfx2dGame.exe',sha256=expected)])))
body=bytearray(target.read_bytes());body[-1]^=1;target.write_bytes(body)
results={}
for name,exe in [('ps51',Path('C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')),
                 ('ps7',Path(shutil.which('pwsh') or 'missing-pwsh'))]:
    assert exe.is_file()
    p=subprocess.run([str(exe),'-NoProfile','-ExecutionPolicy','Bypass','-File',str(out/'Run-PC.ps1'),
        '-FixedAspect','-Fullscreen'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
    (out/(name+'.log')).write_bytes(p.stdout)
    assert p.returncode!=0 and b'Release file changed: pc/gfx2dGame.exe' in p.stdout
    results[name]=dict(status='EXPECTED_RED',returncode=p.returncode,
        reason='modified EXE rejected before game startup; both new switches parsed')
(out/'result.json').write_text(json.dumps(dict(status='PASS',results=results),indent=2)+'\n')
print('PC ENTRY TAMPER PASS PS5.1/PS7',flush=True)
