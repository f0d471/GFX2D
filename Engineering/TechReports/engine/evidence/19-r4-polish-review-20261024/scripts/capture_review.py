"""Render fixed-input current/next R4 frames; verify every PNG against frame CRC."""
from pathlib import Path
import hashlib,json,os,re,shutil,struct,subprocess,time,zipfile,zlib
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[4]
OUTPUT=Path(__file__).parent/('review-'+time.strftime('%Y%m%d-%H%M%S'))
OUTPUT.mkdir()
BASELINE=ROOT/'Main/bit/board_release_game_current_20261024'
old=OUTPUT/'baseline-source'
with zipfile.ZipFile(BASELINE/'source.zip') as archive:archive.extractall(old)
old_assets=old/'Main/assets/build';old_assets.mkdir(parents=True,exist_ok=True)
assert hashlib.sha256((BASELINE/'assets.pak').read_bytes()).digest()==hashlib.sha256((ROOT/'Main/assets/build/assets.pak').read_bytes()).digest()
for name in ('assets.h','assets.pak'):shutil.copy2(ROOT/'Main/assets/build'/name,old_assets/name)

def contract(root,name):
    text=(root/'Main/sw/apps/game/gfx2dGame/src/r4.h').read_text()
    return int(re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+|[0-9]+)u?\b',text,re.M)[1],0)

results={}
previous=None
for candidate in sorted(OUTPUT.parent.glob('review-*/manifest.json'),reverse=True):
    info=json.loads(candidate.read_text())
    if info.get('baseline_manifest_sha256')==hashlib.sha256((BASELINE/'manifest.json').read_bytes()).hexdigest():
        previous=(candidate.parent,info);break
for version,root in [('current',old),('next',ROOT)]:
    if version=='current' and previous:
        location,info=previous
        for scene in ('l2','cup'):
            dest=OUTPUT/version/scene;dest.mkdir(parents=True)
            for name in ('frames.log','build.log'):shutil.copy2(location/version/scene/name,dest/name)
            for entry in info['results']['current-'+scene]['frames']:
                source=location/version/scene/entry['png'];shutil.copy2(source,dest/entry['png'])
                assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256((dest/entry['png']).read_bytes()).digest()
            results['current-'+scene]={**info['results']['current-'+scene],'reused_from':str(location)}
        print('QA REUSED unchanged frozen current frames',flush=True)
        continue
    for scene,count,selected in [('l2',240,[0,30,90,150,210,239]),('cup',60,[0,30,59])]:
        dest=OUTPUT/version/scene;dest.mkdir(parents=True)
        build=dest/'build';(build/'replay').mkdir(parents=True)
        seed=contract(root,'R4_SEED');identity=contract(root,'R4_SCENE_ID')+(scene=='cup')
        ladder=1 if scene=='cup' else 2;asset=contract(root,'R4_ASSET_VERSION')
        keys=[(4 if i%120<15 else 8 if i%120<30 else 0)|(1 if i%60<8 else 2 if i%60<16 else 0) for i in range(count)]
        tape=struct.pack('<4sHH6I',b'R4RP',1,0,30,seed,identity,ladder,asset,count)
        tape+=b''.join(struct.pack('<I',k) for k in keys)
        (build/'replay/r4_replay.h').write_text('static const unsigned char r4_replay_data[]={'+','.join(map(str,tape))+'};\n')
        app=root/'Main/sw/apps/game/gfx2dGame'
        flags='-DGFX2D_START_MODE=4 -DGFX2D_R4_HW=0 -DGFX2D_R4_JUDGE=1 -DGFX2D_R4_REPLAY=1'
        flags+=' -DGFX2D_FRAMES='+str(count)+' -DGFX2D_R4_LADDER='+str(ladder)+' -DGFX2D_R4_SCENE='+str(int(scene=='cup'))
        with (dest/'build.log').open('w') as log:
            subprocess.run(['make','-C',str(app),'-j6','PLATFORM=host','OBJDIR='+os.path.relpath(build,app),
                'PERF=1','EXTRA_CFLAGS='+flags],stdout=log,stderr=subprocess.STDOUT,check=True)
        requested=sorted({frame+offset for frame in selected for offset in range(5)})
        env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1',
             'GFX2D_SNAPSHOT':','.join(map(str,requested)),'GFX2D_SNAPSHOT_DIR':str(dest)}
        with (dest/'frames.log').open('w') as log:
            subprocess.run([build/'gfx2dGame-perf'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        text=(dest/'frames.log').read_text()
        assert re.findall(r'^fail (\d+)\s*$',text,re.M)==['0'] and re.search(r'^done\s*$',text,re.M)
        rows={int(r[0]):(r[2],int(r[1])-1) for r in re.findall(r'^R4CAD (\d+) \d+ \d+ \d+ (\d+)  +\d+ ([0-9a-f]{8}) [0-9a-f]{8}\s*$',text,re.M)}
        assert len(rows)==count
        entries=[]
        for frame in selected:
            expected,snapshot=rows[frame]
            image=Image.open(dest/('snap_%05d.ppm'%snapshot)).convert('RGB')
            raw=bytearray(image.tobytes('raw','BGRX'));raw[3::4]=b'\0'*(640*480)
            crc='%08x'%(zlib.crc32(raw)&0xffffffff)
            assert crc==expected,(version,scene,frame,crc,expected)
            image.save(dest/('frame-%03d.png'%frame))
            entries.append(dict(frame=frame,snapshot=snapshot,crc=crc,png='frame-%03d.png'%frame))
        results[version+'-'+scene]=dict(scene_id=identity,asset_version=asset,
            binary_sha256=hashlib.sha256((build/'gfx2dGame-perf').read_bytes()).hexdigest(),frames=entries)
        print('QA CRC PASS',version,scene,len(entries),flush=True)
for scene,selected in [('l2',[0,30,90,150,210,239]),('cup',[0,30,59])]:
    for frame in selected:
        canvas=Image.new('RGB',(1280,510),'#0b1120');draw=ImageDraw.Draw(canvas)
        for x,version in [(0,'current'),(640,'next')]:
            draw.text((x+12,9),version.upper()+' | '+scene+' | frame '+str(frame)+' | identical keys',fill='white')
            canvas.paste(Image.open(OUTPUT/version/scene/('frame-%03d.png'%frame)),(x,30))
        canvas.save(OUTPUT/(scene+'-compare-%03d.png'%frame))
(OUTPUT/'manifest.json').write_text(json.dumps(dict(baseline=str(BASELINE.relative_to(ROOT)),
    baseline_manifest_sha256=hashlib.sha256((BASELINE/'manifest.json').read_bytes()).hexdigest(),
    results=results,scope='actual host raster frames, fixed identical input; each PNG matches frame CRC; not HDMI photography'),indent=2)+'\n')
print(OUTPUT)
