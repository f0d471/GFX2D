#!/usr/bin/env python3
"""R4 scene 64/32/sanitizer, deterministic full-frame host references and collectors (cup scene removed 2026-11-02)."""
import argparse,hashlib,json,os,sys,subprocess
from check_run import ROOT,session,command,write_json
from r4_checks import build,flags,frames,compare

def committed_model(actual,committed):
    if actual!=committed:raise RuntimeError('current consumer model differs from committed production model')
    return hashlib.sha256(actual).hexdigest()


def main(reuse_flight_models=False,reuse_current_model=False):
    current=session();logs=current.logs('r4');out=current.build('r4-models')
    if os.name=='nt':raise RuntimeError('r4 host check uses WSL')
    sources=['Main/sim/engine/r4/test_scene.c',
        'Main/sw/apps/game/gfx2dGame/src/r4_scene.c',
        'Main/sw/apps/game/gfx2dGame/src/r4_geometry.c',
        'Main/sw/lib/gfx/rast_setup.c','Main/sw/lib/gfx/rast_ref.c','Main/sw/lib/core/fixmath.c',
        'Main/sw/lib/core/sin_tab.c','Main/sw/lib/core/crc32.c','Main/sw/lib/core/xorshift32.c']
    bodies=[]
    for name,extra in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32']),
                       ('64-o3',['-O3','-fsanitize=address,undefined','-fno-sanitize-recover=all']),
                       ('32-o3',['-O3','-m32'])]:
        exe=out/('scene'+name)
        command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',*extra,
            '-I','Main/sw/lib','-I','Main/sw/api','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad','-I','Main/sw/bsp/include',
            '-I','Main/sw/apps/game/gfx2dGame/src','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad',*sources,'-o',exe],logs/('unit'+name+'.build.log'))
        bodies.append(command([exe],logs/('unit'+name+'.log')))
    if any(body!=bodies[0] for body in bodies) or 'R4 SCENE PASS' not in bodies[0]:
        raise RuntimeError('R4 64/32/O2/O3 differ')
    exe=out/'scene-uncached'
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DR4_DISABLE_WALL_CACHE=1',
        '-I','Main/sw/lib','-I','Main/sw/api','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad','-I','Main/sw/bsp/include','-I','Main/sw/apps/game/gfx2dGame/src','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad',
        *sources,'-o',exe],logs/'unit-uncached.build.log')
    if command([exe],logs/'unit-uncached.log')!=bodies[0]:raise RuntimeError('wall cache changed setup records')
    exe=out/'scene-generic-projection'
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DR4_DISABLE_VERTEX_CACHE=1',
        '-I','Main/sw/lib','-I','Main/sw/api','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad','-I','Main/sw/bsp/include','-I','Main/sw/apps/game/gfx2dGame/src','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad',
        *sources,'-o',exe],logs/'unit-generic-projection.build.log')
    if command([exe],logs/'unit-generic-projection.log')!=bodies[0]:
        raise RuntimeError('shared vertex projection changed setup records')
    exe=out/'scene-generic-clipping'
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DRAST_DISABLE_INSIDE_FAST=1',
        '-I','Main/sw/lib','-I','Main/sw/api','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad','-I','Main/sw/bsp/include','-I','Main/sw/apps/game/gfx2dGame/src','-I','Main/sw/services/gfx','-I','Main/sw/ports/myriad',
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
    for name,ladder,scene in [('l1',1,0),('l2',2,0)]:
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
    if reuse_current_model:
        model=ROOT/'Main/sw/apps/game/gfx2dGame/src/r4_models.h'
        committed=subprocess.check_output(['git','-c','safe.directory='+ROOT.as_posix(),
            'show','HEAD:Main/sw/apps/game/gfx2dGame/src/r4_models.h'],cwd=ROOT)
        actual=committed_model(model.read_bytes(),committed)
        damaged=bytearray(committed);damaged[len(damaged)//2]^=1
        try:committed_model(bytes(damaged),committed)
        except RuntimeError:pass
        else:raise RuntimeError('changed production-model identity escaped')
        current.artifact(model)
        write_json(logs/'models.json',dict(status='PASS',sha256=actual,
            scope='unchanged committed production model, current runtime consumers',
            mutation='EXPECTED_RED changed model bytes',
            regeneration='NOT_APPLICABLE: no model or generator adoption'))
    elif reuse_flight_models:
        # SW19 does not adopt another mesh. The production mesh is OBJ-based;
        # its frozen identity replaces the obsolete procedural generator here.
        baseline=json.loads((ROOT/'Main/sim/engine/flight/baseline.json').read_text(encoding='utf-8-sig'))
        model=ROOT/'Main/sw/apps/game/gfx2dGame/src/r4_models.h'
        actual=hashlib.sha256(model.read_bytes()).hexdigest()
        if actual!=baseline['consumer_model_sha256']:raise RuntimeError('frozen consumer model changed')
        current.artifact(model)
        write_json(logs/'models.json',dict(status='PASS',sha256=actual,scope='unchanged frozen consumer model',
            regeneration='NOT_APPLICABLE: model/generator adoption is outside SW19'))
    else:
        command([sys.executable,'Main/tools/assets/gen_r4_models.py','--check'],logs/'models.log')
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
        scope='host and shared scene semantics; physical board uses r4-board',
        model_scope='current committed production model; no regeneration' if reuse_current_model else
            'frozen SW19 identity, no asset regeneration' if reuse_flight_models else 'generator check'))
    print('R4 HOST PASS scenes=2 frames='+str(count),flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--reuse-flight-models',action='store_true')
    parser.add_argument('--reuse-current-model',action='store_true')
    args,_=parser.parse_known_args()
    if args.reuse_flight_models and args.reuse_current_model:parser.error('choose one model scope')
    main(args.reuse_flight_models,args.reuse_current_model)
