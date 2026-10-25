import hashlib,json,subprocess,sys
from pathlib import Path
folder=Path(__file__).resolve().parent;root=folder.parents[3]
evidence=root/'Engineering/TechReports/sw/evidence/11-pc-wide-20261025'
software=root/'Main/sim/build/check-runs/20261025-184831-32bbc76c'
sw=json.loads((software/'result.json').read_text())
window=json.loads((evidence/'window/result.json').read_text())
board=json.loads((evidence/'board/logs/r4-board/result.json').read_text())
layers=json.loads((evidence/'layers/result.json').read_text())
isolation=json.loads((evidence/'layers/isolation.json').read_text())
baseline=json.loads((folder/'fixed-release/comparison.json').read_text())
native=json.loads((folder/'native-fixed-result.json').read_text())
tamper=json.loads((folder/'entry-tamper-final/result.json').read_text())
startup=json.loads((folder/'board-release-20261025-191620/result.json').read_text())
for result in (sw,window,board,layers,isolation,baseline,native,tamper,startup):assert result['status']=='PASS'
for name,digest in isolation['inputs'].items():assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest
pc=folder/'windows-release/gfx2dGame.exe';binary=folder/'target-release/gfx2dGame-O2-perf.bin'
bit=root/board['b7_tree']/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
assert hashlib.sha256(binary.read_bytes()).hexdigest()==startup['bin_sha256']
assert hashlib.sha256(bit.read_bytes()).hexdigest()==startup['bit_sha256']
verification=dict(status='PASS',scope='PC adaptive R4 canvas; fixed 640x480 PC/FPGA reference retained',
    acceptance=dict(software=sw,window=window,layers=layers,board_run='20261025-184825-fcfcb370',
        board_cases=board['results']),
    fixed_baseline=dict(frames=180,pixels='identical',records='identical',native_windows='180 frames identical'),
    displays=[dict(width=2560,height=1600,dpi=144),dict(width=1920,height=1080,dpi=96)],
    display_scope='actual two-display/DPI switching; ultrawide window L1/L2/cup; synthetic disconnect recovery',
    unobserved=['physical monitor unplug/replug','other DPI modes'],
    native_flags='PERF=1 START_MODE=4 R4_HW=0; unlimited; adaptive by default',
    target_flags='PERF=1 START_MODE=4 START_BACKEND=2 R4_HW=1 R4_LADDER=2; unlimited; no JUDGE',
    target_ram_bytes=3264320,board_startup=startup,entry_integrity=tamper,
    pc_sha256=hashlib.sha256(pc.read_bytes()).hexdigest(),
    isolation=dict(copied_files=isolation['copied_files'],original_sources_unchanged=True),
    note='Finite acceptance BINs and unlimited release BIN are distinct; JTAG only; no Flash write; awaiting user visual review.')
path=folder/'release-verification.json';path.write_text(json.dumps(verification,indent=2)+'\n')
args=[sys.executable,str(root/'Main/tools/board/package_game.py'),
    '--name','board_release_game_wide_20261025','--pc',str(pc),
    '--dll',str(pc.parent/'SDL2.dll'),'--bin',str(binary),'--bit',str(bit),
    '--license',str(root/'Main/bit/board_release_game_next_20261024/pc/SDL2-LICENSE.txt'),
    '--verification',str(path),
    '--note','PC mode4 uses an adaptive 640..1792 x 480 canvas with a right-anchored 160px HUD and constant vertical FOV. Fixed 640x480 judge/replay and the B7 board path remain available. Native PC and board programs are unlimited. No RTL or Flash modification in this work point. See verification.json for exact evidence scopes.']
subprocess.run(args,cwd=root,check=True)
print('FINAL GAME PACKAGE PASS',flush=True)
