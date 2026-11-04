#!/usr/bin/env python3
"""HALTTIME production logic, bounded records, deterministic simulation and PC smoke."""
import os
import subprocess
from check_run import ROOT, session, command, write_json


def main():
    current=session();logs=current.logs('sh');out=current.build('sh')
    base=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror']
    for path in ['Main/sw/lib','Main/sw/api','Main/sw/bsp/include','Main/sw/services/gfx',
                 'Main/sw/apps/game/gfx2dGame/src']:
        base+=['-I'+str(ROOT/path)]
    sources=['Main/sim/engine/sh/test_sh.c','Main/sw/lib/core/fixmath.c',
             'Main/sw/lib/core/sin_tab.c','Main/sw/lib/core/xorshift32.c','Main/sw/lib/gfx/rast_setup.c',
             'Main/sw/lib/gfx/rast_ref.c']
    expected=None
    for name,flags in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        exe=out/('sh-'+name)
        command([*base,*flags,*sources,'-o',exe],logs/(name+'.build.log'))
        result=command([exe],logs/(name+'.log'),timeout=120)
        if expected is not None and result!=expected:raise RuntimeError('SH 32/64 mismatch')
        expected=result
    # Reintroduce the inherited 32m spawn regression: the actual gameplay test must fail.
    original=(ROOT/'Main/sw/apps/game/gfx2dGame/src/sh_scene.c').read_text()
    anchor='int32_t x=pos[k][0]*SH_ONE/10,z=pos[k][1]*SH_ONE/10;'
    if anchor not in original:raise RuntimeError('spawn mutation anchor missing')
    mutations={
        'outside-spawn':(anchor,'int32_t x=pos[k][0]*SH_ONE,z=pos[k][1]*SH_ONE/10;'),
        'vulnerable':('++s_hits;s_shield=1;','++s_hits;s_shield=0;'),
        'finite-spawn':('if(alive<SH_ENEMY_MAX){','if(0 && alive<SH_ENEMY_MAX){'),
    }
    for name,(before,after) in mutations.items():
        if before not in original:raise RuntimeError('mutation anchor missing: '+name)
        mutant=out/('sh-'+name+'.c');mutant.write_text(original.replace(before,after,1))
        test=(ROOT/sources[0]).read_text().replace('"../../../sw/apps/game/gfx2dGame/src/sh_scene.c"','"'+str(mutant)+'"')
        fixture=out/('test-'+name+'.c');fixture.write_text(test)
        exe=out/('test-'+name)
        command([*base,fixture,*sources[1:],'-o',exe],logs/(name+'.build.log'))
        result=subprocess.run([exe],cwd=ROOT,text=True,capture_output=True,timeout=30)
        (logs/(name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode!=1 or 'SH FAIL' not in result.stderr:raise RuntimeError('mutation escaped: '+name)
    app=ROOT/'Main/sw/apps/console';host=out/'host'
    command(['make','-C',app,'-j6','PLATFORM=host','OBJDIR='+str(host),'PERF=1'],logs/'host.build.log',timeout=600)
    env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1',
         'GFX2D_SNAPSHOT':'4,30,80','GFX2D_SNAPSHOT_DIR':str(logs)}
    body=command([host/'console-perf','--app','sh','--frames','130','--input-script',
                  '0:0,10:16,11:0,35:65544,50:0,65:262144,66:0,85:896,86:0,105:16384,106:0'],
                  logs/'host.log',env=env,timeout=120)
    if body.count('console scene 3 halttime')!=2 or 'console scene 0 ladder' not in body or 'errors 0' not in body:
        raise RuntimeError('actual console SH / corridor / SH smoke incomplete')
    from console_checks import clean,catalog
    clean(body);catalog(body)
    # Exercise the actual runtime's viewmodel depth reset at the inherited white-wall pose.
    wall=logs/'wall';wall.mkdir(exist_ok=True)
    wall_env={**env,'GFX2D_SNAPSHOT':'230','GFX2D_SNAPSHOT_DIR':str(wall)}
    wall_body=command([host/'console-perf','--app','sh','--frames','240','--input-script',
        '0:17,45:65560,90:32788,135:17,180:262160,181:16,220:1'],
        logs/'wall.log',env=wall_env,timeout=120)
    clean(wall_body)
    from PIL import Image
    im=Image.open(wall/'snap_00230.ppm').convert('RGB')
    dark=sum(max(im.getpixel((x,y)))<80 for y in range(300,430) for x in range(380,600))
    if dark<100:raise RuntimeError('actual near-wall runtime hid the first-person gun')
    (logs/'wall-pixels.log').write_text('SH WALL PIXELS PASS visible gun pixels='+str(dark)+'\n')
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',checks=['production gameplay','64/32 parity',
        'ASan/UBSan','endless 9000 ticks','invincible sustained hits','automatic and manual reload',
        'world-time fragments','2048 host-owned records and bins','viewport padding','determinism',
        'actual console SH smoke','actual near-wall viewmodel pixels'],mutation=['EXPECTED_RED '+name for name in mutations],
        scope='PC software; no FPGA timing or hardware overlay claim'))
    print('SH PASS')


if __name__=='__main__':main()
