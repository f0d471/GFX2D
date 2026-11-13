#!/usr/bin/env python3
"""Check the actual target scheduler independently of gameplay and PC screenshots."""
import subprocess
from check_run import ROOT, session, command, write_json

PIPELINE_MUTATIONS = {'record-base': ('shared + rec_off + first*RAST_RECORD_BYTES',
                 'shared + (rec_off&0u) + R4_REC_OFF + first*RAST_RECORD_BYTES'),
 'record-overwrite': ('const int next=had_frame?(cur^1):cur;', 'const int next=cur;'),
 'early-present': ('if(had_frame && s_pipe_buf[cur].gun_bytes) {',
                   'if(had_frame){board_frame f;uint32_t ticket;int '
                   'shown=s_pipe_buf[cur].dst_slot;board_frame_acquire(shown,0,&f);board_present_frame_request(shown,&f,&ticket);}\n'
                   '    if(had_frame && s_pipe_buf[cur].gun_bytes) {'),
 'leave-drain': ('if(gfx_engine_wait(&code,&index,&tag) || code) {',
                 'if((void)index,(void)tag,code) {')}


def _run(current):
    logs=current.logs('pipeline');out=current.build('pipeline')
    base=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror']
    for path in ['Main/sw/lib','Main/sw/api','Main/sw/bsp/include','Main/sw/services/gfx',
                 'Main/sw/apps/game/gfx2dGame/src']:
        base+=['-I'+str(ROOT/path)]
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
    pipeline_mutations=PIPELINE_MUTATIONS
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

    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,
        scope='actual target scheduler with delayed engine/display models; no board claim',
        checks=['64/32','ASan/UBSan','A/B records and sublists','in-flight ownership',
                'prime and input generations','present after draw','failure and leave drain'],
        mutations=['EXPECTED_RED '+name for name in PIPELINE_MUTATIONS]))


def run_pipeline(current):
    current.perform('pipeline.models',lambda:_run(current))


def main():
    current=session();run_pipeline(current);current.verify()
    print('PIPELINE PASS 64/32 ownership and four mutations')


if __name__=='__main__':main()
