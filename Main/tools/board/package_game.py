#!/usr/bin/env python3
"""Freeze a PC/FPGA game pair, assets, deployment tools and source into Main/bit."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True)
    ap.add_argument('--pc', type=Path, required=True)
    ap.add_argument('--dll', type=Path, required=True)
    ap.add_argument('--bin', type=Path, required=True)
    ap.add_argument('--bit', type=Path, required=True)
    ap.add_argument('--license', type=Path)
    ap.add_argument('--qa',type=Path)
    ap.add_argument('--verification',type=Path)
    ap.add_argument('--content',type=Path)
    ap.add_argument('--bit-manifest',type=Path)
    ap.add_argument('--note', required=True)
    args = ap.parse_args()
    if not args.name.startswith('board_release_') or Path(args.name).name != args.name:
        ap.error('name must be one board_release_* directory name')
    output = ROOT / 'Main/bit' / args.name
    if output.exists():
        ap.error('refusing to replace an existing release')
    inputs = {
        'pc/gfx2dGame.exe': args.pc,
        'pc/SDL2.dll': args.dll,
        'board/gfx2dGame.bin': args.bin,
        'board/gfx2d_ti60.bit': args.bit,
        'assets.pak': ROOT / 'Main/assets/build/assets.pak',
        'pc/assets.pak': ROOT / 'Main/assets/build/assets.pak',
        'sdk/cpu0.yaml': ROOT / 'Main/sdk/cpu0.yaml',
        'tools/release_launch.py': Path(__file__).with_name('release_launch.py'),
    }
    for name in ('board_run.py', 'game_control.py', 'input_forward.py', 'gfx2d_debug.cfg'):
        inputs['tools/board/' + name] = Path(__file__).with_name(name)
    if args.license:inputs['pc/SDL2-LICENSE.txt']=args.license
    if args.verification:inputs['verification.json']=args.verification
    if args.qa:
        inputs['qa/manifest.json']=args.qa/'manifest.json'
        for path in args.qa.glob('*-compare-*.png'):inputs['qa/'+path.name]=path
        for version in ('current','next'):
            for scene in ('l2',):   # cup scene removed 2026-11-02
                source=args.qa/version/scene
                for path in [*source.glob('frame-*.png'),source/'frames.log']:
                    inputs['qa/'+version+'/'+scene+'/'+path.name]=path
    for suffix in ('.elf','.map'):
        if args.bin.with_suffix(suffix).is_file():
            inputs['board/gfx2dGame'+suffix]=args.bin.with_suffix(suffix)
    if (args.pc.parent/'build.json').is_file():inputs['pc/build.json']=args.pc.parent/'build.json'
    missing = [str(p) for p in inputs.values() if not p.is_file()]
    if missing:
        ap.error('missing inputs: ' + ', '.join(missing))
    board_console=b'console protocol 2' in args.bin.read_bytes()
    pc_console=b'console protocol 2' in args.pc.read_bytes()
    if board_console!=pc_console:
        ap.error('PC and board must use the same product frontend')
    frontend='console' if board_console else 'legacy'
    content=None;hardware=None
    if frontend=='console':
        board_content=args.content or args.bin.parent/'catalog/content.json'
        if not board_content.is_file():board_content=args.bin.parent/'content.json'
        pc_content=args.pc.parent/'content.json'
        if not pc_content.is_file():pc_content=args.pc.parent/'catalog/content.json'
        if not board_content.is_file() or not pc_content.is_file():ap.error('console package requires PC and firmware content manifests')
        from console_catalog import CAPS_BIT,validate_selection
        content=validate_selection(json.loads(board_content.read_text(encoding='utf-8')))
        pc_selected=json.loads(pc_content.read_text(encoding='utf-8'))
        if content!=pc_selected:ap.error('PC and firmware application selections differ')
        fingerprint=content['catalog_sha256'].encode('ascii')
        if fingerprint not in args.bin.read_bytes() or fingerprint not in args.pc.read_bytes():ap.error('content identity not embedded in both binaries')
        bit_manifest=args.bit_manifest or args.bit.parent/'manifest.json'
        if not bit_manifest.is_file():ap.error('console package requires a bitstream identity manifest')
        hardware=json.loads(bit_manifest.read_text(encoding='utf-8'))
        if hardware['bit_sha256']!=digest(args.bit):ap.error('bitstream identity mismatch')
        caps=int(hardware['caps'],16)
        unsupported=[a['key'] for a in content['apps'] if any(not caps&(1<<CAPS_BIT[f]) for f in a['target_requires'])]
        if unsupported:ap.error('selected bitstream cannot run packaged applications: '+','.join(unsupported))
        if args.bin.stat().st_size>content['image_limit_bytes']:ap.error('firmware exceeds boot image budget')
        if inputs['assets.pak'].stat().st_size>content['asset_limit_bytes']:ap.error('assets exceed Flash partition budget')
        inputs['content.json']=board_content
        inputs['board/hardware.json']=bit_manifest
        firmware=args.bin.parent/'firmware.json'
        if not firmware.is_file():ap.error('firmware build identity missing')
        built=json.loads(firmware.read_text(encoding='utf-8'))
        if built['files'][args.bin.name]['sha256']!=digest(args.bin) or built['assets']['sha256']!=digest(inputs['assets.pak']):
            ap.error('firmware/asset build identities differ from package inputs')
        inputs['board/firmware.json']=firmware
    for name, source in inputs.items():
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
        if digest(dest) != digest(source):
            raise RuntimeError('copy mismatch: ' + name)
    git = ['git', '-c', 'safe.directory=' + ROOT.as_posix()]
    files = subprocess.check_output(git + ['ls-files', '-z', '--cached', '--others',
        '--exclude-standard', '--', 'Main'], cwd=ROOT).decode('utf-8').split('\0')
    source_names = [n for n in files if n and (ROOT / n).is_file() and
        not any(p in ('logs', 'outflow') or p.startswith(('build_', 'build-', 'board_release_'))
                or p == 'build' for p in Path(n).parts)]
    with zipfile.ZipFile(output / 'source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in source_names:
            archive.write(ROOT / name, name)
    (output / 'source.diff').write_bytes(subprocess.check_output(git + ['diff', '--', 'Main'], cwd=ROOT))
    (output / 'Run-PC.ps1').write_text('''param([switch]$Fullscreen, [switch]$FixedAspect)
$ErrorActionPreference = 'Stop'
function Get-ReleaseHash([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    if ($entry.path -like 'pc/*' -or $entry.path -eq 'assets.pak') {
        $path = Join-Path $PSScriptRoot $entry.path
        if ((Get-ReleaseHash $path) -ne $entry.sha256) {
            throw ('Release file changed: ' + $entry.path)
        }
    }
}
$saved = @{}
foreach ($name in @('GFX2D_ASSETS_PAK','GFX2D_SCALE_FILTER','GFX2D_FULLSCREEN','GFX2D_ASPECT')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
try {
    $env:GFX2D_ASSETS_PAK = Join-Path $PSScriptRoot 'assets.pak'
    $env:GFX2D_SCALE_FILTER = 'linear'
    $env:GFX2D_FULLSCREEN = if ($Fullscreen) { '1' } else { '0' }
    $env:GFX2D_ASPECT = if ($FixedAspect) { 'fixed' } else { 'auto' }
    Start-Process -FilePath (Join-Path $PSScriptRoot 'pc/gfx2dGame.exe') -WorkingDirectory (Join-Path $PSScriptRoot 'pc') -WindowStyle Normal
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
''', encoding='utf-8-sig')
    (output / 'Run-Board.ps1').write_text('''param([string]$Port, [string]$EfinityHome = $env:EFINITY_HOME, [switch]$NoPanel, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$candidates = @()
if ($EfinityHome) { $candidates += $EfinityHome }
foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
    $base = Join-Path $drive.Root 'Efinity'
    if (Test-Path -LiteralPath $base) {
        $candidates += @(Get-ChildItem -LiteralPath $base -Directory | Sort-Object Name -Descending | ForEach-Object { $_.FullName })
    }
}
$python = $null
foreach ($candidate in $candidates) {
    $exe = Join-Path $candidate 'python311/bin/python.exe'
    if (Test-Path -LiteralPath $exe) { $python = $exe; break }
}
if (-not $python) { throw 'Efinity Python not found; specify -EfinityHome.' }
$saved = @{}
foreach ($name in @('PYTHONHOME','PYTHONIOENCODING','PYTHONDONTWRITEBYTECODE')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:PYTHONHOME = Split-Path -Parent (Split-Path -Parent $python)
    $env:PYTHONIOENCODING = 'utf-8'; $env:PYTHONDONTWRITEBYTECODE = '1'
    $arguments = @((Join-Path $PSScriptRoot 'tools/release_launch.py'), '--root', $PSScriptRoot)
    if ($Port) { $arguments += @('--port', $Port) }
    if ($NoPanel) { $arguments += '--no-panel' }
    if ($CheckOnly) { $arguments += '--check-only' }
    & $python @arguments
    if ($LASTEXITCODE -ne 0) { throw ('Board launch failed: ' + $LASTEXITCODE) }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
''', encoding='utf-8-sig')
    for label in ('PC', 'Board'):
        (output / ('Run-' + label + '.cmd')).write_text(
            '@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-' +
            label + '.ps1" %*\r\nif errorlevel 1 pause\r\n', encoding='ascii')
    (output/'Run-PC-Fullscreen.cmd').write_text(
        '@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-PC.ps1" -Fullscreen\r\nif errorlevel 1 pause\r\n',encoding='ascii')
    (output / 'README.md').write_text(
        '# ' + args.name + '\n\n' + args.note + '\n\n'
        '双击 `Run-PC.cmd` 启动原生 Windows 游戏；双击 `Run-Board.cmd` 重新加载本包 bit/BIN 并打开控制面板。\n'
        '`Run-PC-Fullscreen.cmd` 直接全屏启动；F11/Alt+Enter切全屏，F1/F2/F3调窗口，F4切平滑/像素放大。\n'
        '模式4默认自动适应宽屏；固定640×480对照可运行 `Run-PC.ps1 -FixedAspect`。\n'
        'PC 不需要板卡或 Efinity。板端需要 Efinity 2026.1/IDE、FT4232H 驱动和已装入 Flash 的原配素材包；本包保留 assets.pak。\n'
        '板端仅 JTAG 重载，不写 Flash。UART 自动定位 C 通道；多板时用 Run-Board.ps1 -Port 指定。\n'
        '加载前关闭占用同一串口的控制面板；程序退出/断电后可再次运行本包入口恢复。\n'+
        ('PC 与板端使用包内 content.json 的同一应用集合。上下选择，Enter/Space确认，Esc返回；数字键为该集合的可选快捷入口。\n'
         '板载四键：左右同按确认，上下同按返回；游戏中确认可使用炸弹。Shift切阶梯，Tab切后端。\n'
         if frontend=='console' else
         '旧开发入口：4进入三维、三维内再按4切茶杯；Shift切L1/L2，Tab切后端。\n')+
        '`source.zip` 是此包冻结时的源码，`source.diff` 保留当时相对 Git 的变更。\n'
        '文件身份及旧/新版本的实际构建说明见 manifest.json，运行日志写 runtime/。\n', encoding='utf-8')
    manifest = dict(name=args.name, note=args.note, deployment='JTAG; no Flash write',frontend=frontend,
        board_bit='board/gfx2d_ti60.bit',board_bin='board/gfx2dGame.bin',
        content=content,hardware=hardware,
        hardware_scope='reused bitstream; packaging does not rebuild or accept current RTL',
        bit_sha256=digest(args.bit), source_head=subprocess.check_output(git + ['rev-parse', 'HEAD'], cwd=ROOT).decode().strip(),
        rtl={p.relative_to(ROOT).as_posix(): digest(p) for p in (ROOT / 'Main/rtl').rglob('*') if p.suffix in ('.v','.vh')},
        files=[dict(path=p.relative_to(output).as_posix(), bytes=p.stat().st_size, sha256=digest(p))
               for p in sorted(output.rglob('*')) if p.is_file()])
    (output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(output)


if __name__ == '__main__':
    main()
