#!/usr/bin/env python3
"""R4 scene/cup 64/32/sanitizer, deterministic full-frame host references and collectors."""
import json,os,sys
from check_run import ROOT,session,command,write_json
from r4_checks import build,flags,frames,compare

def main():
    current=session();logs=current.logs('r4');out=current.build('r4-models')
    if os.name=='nt':raise RuntimeError('r4 host check uses WSL')
    sources=['Main/sim/engine/r4/test_scene.c',
        'Main/sw/apps/game/gfx2dGame/src/r4_scene.c','Main/sw/apps/game/gfx2dGame/src/r4_cup.c',
        'Main/sw/apps/game/gfx2dGame/src/r4_geometry.c',
        'Main/sw/lib/gfx/rast_setup.c','Main/sw/lib/gfx/rast_ref.c','Main/sw/lib/core/fixmath.c',
        'Main/sw/lib/core/sin_tab.c','Main/sw/lib/core/crc32.c','Main/sw/lib/core/xorshift32.c']
    bodies=[]
    for name,extra in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        exe=out/('scene'+name)
        command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',*extra,
            '-I','Main/sw/lib','-I','Main/sw/bsp/include',
            '-I','Main/sw/apps/game/gfx2dGame/src',*sources,'-o',exe],logs/('unit'+name+'.build.log'))
        bodies.append(command([exe],logs/('unit'+name+'.log')))
    if bodies[0]!=bodies[1] or 'R4 SCENE PASS' not in bodies[0]:raise RuntimeError('R4 64/32 differ')
    exe=out/'scene-uncached'
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DR4_DISABLE_WALL_CACHE=1',
        '-I','Main/sw/lib','-I','Main/sw/bsp/include','-I','Main/sw/apps/game/gfx2dGame/src',
        *sources,'-o',exe],logs/'unit-uncached.build.log')
    if command([exe],logs/'unit-uncached.log')!=bodies[0]:raise RuntimeError('wall cache changed setup records')
    exe=out/'scene-generic-projection'
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DR4_DISABLE_VERTEX_CACHE=1',
        '-I','Main/sw/lib','-I','Main/sw/bsp/include','-I','Main/sw/apps/game/gfx2dGame/src',
        *sources,'-o',exe],logs/'unit-generic-projection.build.log')
    if command([exe],logs/'unit-generic-projection.log')!=bodies[0]:
        raise RuntimeError('shared vertex projection changed setup records')
    exe=out/'scene-generic-clipping'
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DRAST_DISABLE_INSIDE_FAST=1',
        '-I','Main/sw/lib','-I','Main/sw/bsp/include','-I','Main/sw/apps/game/gfx2dGame/src',
        *sources,'-o',exe],logs/'unit-generic-clipping.build.log')
    if command([exe],logs/'unit-generic-clipping.log')!=bodies[0]:
        raise RuntimeError('inside-triangle fast path changed setup records')
    sys.path.insert(0,str(ROOT/'Main/tools/board'))
    from input_forward import packet,InputForwarder
    for k in range(1024):
        a,b=packet(k);decoded=(a&127)|((b&7)<<7)
        assert decoded==k and b>>3==((k^(k>>4)^(k>>8))&15)
    class Keyboard:
        held=True
        def GetForegroundWindow(self):return 1
        def GetAsyncKeyState(self,k):return 0x8000 if self.held and k==ord('4') else 0
    forwarder=InputForwarder.__new__(InputForwarder)
    forwarder.user32=Keyboard();forwarder.focus=1;forwarder.xinput=None
    assert forwarder.sample()==0x380
    forwarder.user32.held=False;assert forwarder.sample()==0
    count=12 if current.profile=='quick' else 60
    results={}
    for name,ladder,scene in [('l1',1,0),('l2',2,0),('cup',1,1)]:
        scene_count=240 if name=='l2' and current.profile!='quick' else count
        binary=build(current,'host-'+name,flags(ladder,scene,frames=scene_count),perf=True)
        body=command([binary],logs/(name+'.log'),env={**os.environ,
            'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1'})
        results[name]=frames(body,scene_count)
        if not any(int(x['records'],16) for x in results[name]):raise RuntimeError('empty R4 geometry')
        damaged=body.replace('R4CAD 0 ','R4CAD 1 ',1)
        try:frames(damaged,scene_count)
        except RuntimeError:pass
        else:raise RuntimeError('collector accepted duplicate frame')
    write_json(logs/'expected.json',results)
    # Actual geometry must depend on input and every sampled frame must differ.
    for name,rows in results.items():
        if len({r['records'] for r in rows})<len(rows)//2:raise RuntimeError('frozen geometry '+name)
    command([sys.executable,'Main/tools/assets/gen_r4_atlas.py','--check'],logs/'atlas.log')
    command([sys.executable,'Main/tools/assets/gen_r4_models.py','--check'],logs/'models.log')
    import subprocess
    p=subprocess.run([sys.executable,'Main/tools/assets/gen_r4_atlas.py','--check','--inject'],
                     capture_output=True,text=True,cwd=ROOT)
    (logs/'atlas-injection.log').write_text(p.stdout+p.stderr,encoding='utf-8')
    if p.returncode!=1:raise RuntimeError('atlas mutation escaped')
    # A pixel corruption must be rejected independently of the setup identity.
    body=(logs/'l2.log').read_text(encoding='utf-8')
    first=results['l2'][0]['crc'];bad=body.replace(' '+first+' ',' '+f'{int(first,16)^1:08x}'+' ',1)
    try:compare(bad,results['l2'])
    except RuntimeError:pass
    else:raise RuntimeError('collector accepted pixel corruption')
    record=build(current,'recording',flags(frames=8,judge=False)+' -DGFX2D_R4_RECORD=1')
    body=command([record],logs/'recording.log',env={**os.environ,'SDL_VIDEODRIVER':'dummy',
        'SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1'})
    from r4_checks import recording
    recording(body,8)
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,
        frames_per_scene={k:len(v) for k,v in results.items()},scenes=list(results),unit=bodies[0].strip(),
        scope='host and shared scene semantics; physical board uses r4-board'))
    print('R4 HOST PASS scenes=3 frames='+str(count),flush=True)

if __name__=='__main__':main()
