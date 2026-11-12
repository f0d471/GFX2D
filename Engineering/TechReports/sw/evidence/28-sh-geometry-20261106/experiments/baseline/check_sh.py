#!/usr/bin/env python3
"""HALTTIME production logic, bounded records, deterministic simulation and PC smoke."""
import os
import subprocess
from check_run import ROOT, session, command, write_json


def main():
    current=session();logs=current.logs('sh');out=current.build('sh')
    command(['python3',ROOT/'Main/tools/assets/gen_sh_mesh.py','--check'],logs/'mesh-generator.log')
    base=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror']
    for path in ['Main/sw/lib','Main/sw/api','Main/sw/bsp/include','Main/sw/services/gfx',
                 'Main/sw/apps/game/gfx2dGame/src']:
        base+=['-I'+str(ROOT/path)]
    sources=['Main/sim/engine/sh/test_sh.c','Main/sw/lib/core/fixmath.c',
             'Main/sw/lib/core/sin_tab.c','Main/sw/lib/core/xorshift32.c','Main/sw/lib/gfx/rast_setup.c',
             'Main/sw/lib/gfx/rast_ref.c','Main/sw/apps/game/gfx2dGame/src/r4_geometry.c']
    expected=None
    for name,flags in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        exe=out/('sh-'+name)
        command([*base,*flags,*sources,'-o',exe],logs/(name+'.build.log'))
        result=command([exe],logs/(name+'.log'),timeout=120)
        if expected is not None and result!=expected:raise RuntimeError('SH 32/64 mismatch')
        expected=result
    exe=out/'sh-generic'
    command([*base,'-DSH_DISABLE_VERTEX_CACHE=1','-DSH_DISABLE_SCENE_CACHE=1',*sources,'-o',exe],logs/'generic.build.log')
    generic=command([exe],logs/'generic.log',timeout=120)
    geometry=lambda body:next(line for line in body.splitlines() if line.startswith('SH GEOMETRY PASS'))
    if geometry(generic)!=geometry(expected):raise RuntimeError('SH cache changed record bytes versus generic path')
    # Reintroduce the inherited 32m spawn regression: the actual gameplay test must fail.
    original=(ROOT/'Main/sw/apps/game/gfx2dGame/src/sh_scene.c').read_text()
    anchor='int32_t x=pos[k][0]*SH_ONE/10,z=pos[k][1]*SH_ONE/10;'
    if anchor not in original:raise RuntimeError('spawn mutation anchor missing')
    mutations={
        'outside-spawn':(anchor,'int32_t x=pos[k][0]*SH_ONE,z=pos[k][1]*SH_ONE/10;'),
        'vulnerable':('++s_hits;s_shield=1;','++s_hits;s_shield=0;'),
        'finite-spawn':('if(alive<SH_ENEMY_MAX){','if(0 && alive<SH_ENEMY_MAX){'),
        'zero-normal':('uint32_t maximum=0;','v[0]=v[1]=v[2]=0;return;uint32_t maximum=0;'),
        'stale-camera':('s_room_cache.yaw==s_yaw','1'),
        'facet-gradient':('screen[j].color=color;inside&=t->inside;',
                          'screen[j].color=color+j*0x010101u;inside&=t->inside;'),
        'negative-half':('q+=n<0?r>d/2u:r>=(d+1u)/2u;',
                         'q+=r>=(d+1u)/2u;'),
        'range-publication':('memcpy(s_range_pending[i].cache,s_ranges+2u*s_range_pending[i].first,2u*s_range_pending[i].count);',
                             '(void)s_range_pending[i].cache;'),
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
    # Execute the actual target pipeline with a delayed engine/display model.
    pipeline_sources=['Main/sw/lib/gfx/rast_setup.c','Main/sw/lib/core/fixmath.c',
        'Main/sw/lib/core/sin_tab.c','Main/sw/lib/core/format.c','Main/sw/lib/core/logq.c',
        'Main/sw/lib/core/replay_r4.c','Main/sw/bsp/memmap.c']
    pipeline_flags=[*base,'-IMain/sw/ports/myriad','-IMain/assets/build',
                    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    fixture=ROOT/'Main/sim/engine/sh/test_pipeline.c'
    for name,extra in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        exe=out/('pipeline-'+name)
        command([*pipeline_flags,*extra,fixture,*pipeline_sources,'-o',exe],logs/('pipeline-'+name+'.build.log'))
        command([exe],logs/('pipeline-'+name+'.log'))
    runtime=(ROOT/'Main/sw/ports/myriad/r4_runtime.c').read_text()
    pipeline_mutations={
        'record-base':('shared + rec_off + first*RAST_RECORD_BYTES',
                       'shared + (rec_off&0u) + R4_REC_OFF + first*RAST_RECORD_BYTES'),
        'record-overwrite':('const int next=had_frame?(cur^1):cur;','const int next=cur;'),
        'early-present':('if(had_frame && s_pipe_buf[cur].gun_bytes) {',
            'if(had_frame){board_frame f;uint32_t ticket;int shown=s_pipe_buf[cur].dst_slot;'
            'board_frame_acquire(shown,0,&f);board_present_frame_request(shown,&f,&ticket);}\n'
            '    if(had_frame && s_pipe_buf[cur].gun_bytes) {'),
        'leave-drain':('if(gfx_engine_wait(&code,&index,&tag) || code) {','if((void)index,(void)tag,code) {'),
    }
    for name,(before,after) in pipeline_mutations.items():
        if before not in runtime:raise RuntimeError('pipeline mutation anchor missing '+name)
        mutant=out/('runtime-'+name+'.c');mutant.write_text(runtime.replace(before,after,1))
        test=out/('pipeline-'+name+'.c')
        test.write_text(fixture.read_text().replace('"../../../sw/ports/myriad/r4_runtime.c"','"'+str(mutant)+'"'))
        exe=out/('pipeline-'+name)
        command([*pipeline_flags,test,*pipeline_sources,'-o',exe],logs/('pipeline-'+name+'.build.log'))
        result=subprocess.run([exe],cwd=ROOT,text=True,capture_output=True,timeout=30)
        (logs/('pipeline-'+name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode!=1 or 'SH PIPE FAIL' not in result.stderr:
            raise RuntimeError('pipeline mutation escaped '+name)
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
    active=command([host/'console-perf','--app','sh','--frames','720','--input-script',
        '0:17,90:24,180:18,270:20,360:17,450:24,540:18,630:20'],
        logs/'active.log',env={**env,'GFX2D_SNAPSHOT':'140,260,500'},timeout=240)
    clean(active)
    if 'frames 720 presents 720 errors 0' not in active:raise RuntimeError('continuous SH move/fire runtime failed')
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
        'world-time fragments','30Hz elapsed-clock batches','unit normals and tiny vectors',
        '50000 exact projection divisions','100000 signed depth divisions and half ties',
        '10000 byte-identical flat triangles','33 baked faceted poses and flat RGB',
        'generic/cache record byte equality','cache arena guards',
        'next-frame strip-range reuse and invalidation','atomic range publication',
        'actual target pipeline A/B ownership and three submission states',
        'present after draw, prime, input generation and failure drains',
        '2048 records and bins','viewport padding','determinism',
        'actual console SH smoke','actual near-wall viewmodel pixels'],
        mutation=['EXPECTED_RED '+name for name in [*mutations,*('pipeline-'+n for n in pipeline_mutations)]],
        scope='PC software; no FPGA timing or hardware overlay claim'))
    print('SH PASS')


if __name__=='__main__':main()
