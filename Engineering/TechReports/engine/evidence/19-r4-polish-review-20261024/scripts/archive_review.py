"""Archive actual CRC-bound review images and native compiler cross-checks once."""
import hashlib,json,os,shutil,sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'Main/tools/check'))
from r4_checks import frames
BASE=Path(__file__).parent
SOURCE=BASE/'review-20261024-173319'
DEST=ROOT/'Engineering/TechReports/engine/evidence/19-r4-polish-review-20261024'
assert not DEST.exists()
software=ROOT/'Main/sim/build/check-runs/20261024-173917-188abb7f'
board=ROOT/'Main/sim/build/check-runs/20261024-192236-f8d2026c'
expected=json.loads((software/'logs/r4/expected.json').read_text())
meta=json.loads((SOURCE/'manifest.json').read_text())
checks=[]
for scene in ('l2','cup'):
    body=(board/'logs/r4-board'/(scene+'.serial.log')).read_text(encoding='utf-8-sig')
    body=body.split('# --- 程序已加载，以下才是它的输出 ---',1)[1]
    hardware=frames(body,len(expected[scene]))
    for entry in meta['results']['next-'+scene]['frames']:
        i=entry['frame'];assert entry['crc']==expected[scene][i]['crc']==hardware[i]['crc']
        checks.append(dict(scene=scene,frame=i,crc=entry['crc'],status='PASS'))
native=json.loads((BASE/'native-pixels/result.json').read_text())
assert native['status']=='PASS'
DEST.mkdir(parents=True)
copied=[]
def copy(src,relative):
    dest=DEST/relative;dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(src,dest)
    a=hashlib.sha256(src.read_bytes()).hexdigest();b=hashlib.sha256(dest.read_bytes()).hexdigest()
    assert a==b
    copied.append(dict(path=str(relative),bytes=dest.stat().st_size,sha256=b))
copy(SOURCE/'manifest.json',Path('qa/manifest.json'))
for src in SOURCE.glob('*-compare-*.png'):copy(src,Path('qa')/src.name)
for version in ('current','next'):
    for scene in ('l2','cup'):
        for src in (SOURCE/version/scene).glob('frame-*.png'):copy(src,Path('qa')/version/scene/src.name)
        copy(SOURCE/version/scene/'frames.log',Path('qa')/version/scene/'frames.log')
for src in (BASE/'native-pixels').rglob('*'):
    if src.is_file() and src.name in ('result.json','frames.log','build.log','build.json'):
        copy(src,Path('native-pixels')/src.relative_to(BASE/'native-pixels'))
for name in ('capture_review.py','native_pixels.py','archive_review.py'):
    copy(BASE/name,Path('scripts')/name)
summary=dict(status='PASS',qa_images=18,final_images_bound_to_host_and_board=checks,
    native=native,software_run=software.name,board_run=board.name,
    scope='actual raster PNGs; final selected frames match host and hardware; not HDMI photographs')
(DEST/'verification.json').write_text(json.dumps(summary,indent=2)+'\n')
(DEST/'copy_manifest.json').write_text(json.dumps(copied,indent=2)+'\n')
(DEST/'README.md').write_text('# 软件视觉升级检阅图\n\n旧图来自冻结旧源码，同一逻辑输入；新版每张选图CRC同时匹配正式宿主和实板帧。\n'
    '18张原图和9张对照图为实际软件光栅输出；不是HDMI实拍。\n'
    'native-pixels记录原生Windows三场景360帧与WSL验收参考逐帧/逐记录一致。\n'
    'scripts为原始取样脚本，复现时放回Main/sim/build/r4-visual-upgrade-20261024并使用对应冻结源码。\n'
    'verification.json是图像与正式运行的关联；copy_manifest.json记录复制后身份。\n',encoding='utf-8')
print('REVIEW ARCHIVE PASS',DEST)
