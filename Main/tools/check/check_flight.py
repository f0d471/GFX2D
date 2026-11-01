#!/usr/bin/env python3
"""RFL3 damped controls, world anchors, uncached fixture and cache mutations."""
import hashlib
import json
import os
import subprocess
import sys
from check_run import ROOT,session,command,write_json


def main():
    if os.name=='nt':raise RuntimeError('flight software checks use WSL')
    current=session();logs=current.logs('flight');out=current.build('flight')
    game='Main/sw/apps/game/gfx2dGame/src/'
    sources=['Main/sim/engine/flight/test_flight.c',game+'r4_scene.c',game+'r4_geometry.c',
             'Main/sw/ports/myriad/r4_runtime.c','Main/sw/lib/gfx/rast_setup.c',
             'Main/sw/lib/gfx/rast_ref.c','Main/sw/lib/core/fixmath.c',
             'Main/sw/lib/core/sin_tab.c','Main/sw/lib/core/crc32.c','Main/sw/lib/core/xorshift32.c']
    base=['gcc','-std=gnu11','-O3','-g','-Wall','-Wextra','-Werror','-DR4_FLIGHT_TEST=1',
          '-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    for path in ['Main/sw/lib','Main/sw/api','Main/sw/services/gfx','Main/sw/bsp/include',
                 'Main/sw/ports/myriad','Main/assets/build',game]:base+=['-I',path]
    frames=240 if current.profile=='quick' else 1801
    profile=current.profile.replace('release','acceptance')
    fixture=ROOT/'Main/sim/engine/flight'/('records-'+profile+'.txt')
    provenance=json.loads((fixture.parent/'baseline.json').read_text())
    from r4_checks import contract
    if provenance.get('scene_id')!=contract('R4_FLIGHT_SCENE_ID'):
        raise RuntimeError('flight fixture belongs to a different gameplay contract')
    if hashlib.sha256(fixture.read_bytes()).hexdigest()!=provenance['fixtures'][profile]['sha256']:
        raise RuntimeError('frozen flight fixture identity changed')
    expected=fixture.read_text();current.artifact(fixture)
    variants=[('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32']),
              ('generic',['-DR4_DISABLE_VERTEX_CACHE=1'])]
    for name,flags in variants:
        exe=out/('flight-'+name)
        command([*base,*flags,*sources,'-o',exe],logs/(name+'.build.log'))
        body=command([exe,str(frames)],logs/(name+'.log'),timeout=300)
        if body!=expected:raise RuntimeError('flight differs from frozen RFL3 uncached fixture: '+name)
    # Viewport reuse without invalidating projected records must be detected.
    exe=out/'stale-view'
    command([*base,'-DR4_INJECT_FLIGHT_STALE_VIEW=1',*sources,'-o',exe],logs/'mutation.build.log')
    result=subprocess.run([exe,'12'],cwd=ROOT,text=True,capture_output=True,timeout=90)
    (logs/'mutation.log').write_text(result.stdout+result.stderr)
    if result.returncode!=1 or 'FLIGHT FAIL viewport cache invalidation' not in result.stderr:
        raise RuntimeError('stale viewport mutation was not detected at the expected check')
    exe=out/'reverse-world'
    command([*base,'-DR4_INJECT_FLIGHT_REVERSE=1',*sources,'-o',exe],logs/'reverse.build.log')
    result=subprocess.run([exe,'12'],cwd=ROOT,text=True,capture_output=True,timeout=90)
    (logs/'reverse.log').write_text(result.stdout+result.stderr)
    if result.returncode!=1 or 'FLIGHT FAIL world anchor direction' not in result.stderr:
        raise RuntimeError('reversed world movement was not detected')
    exe=out/'unsigned-step'
    command([*base,'-DR4_INJECT_FLIGHT_UNSIGNED_STEP=1',*sources,'-o',exe],logs/'unsigned.build.log')
    result=subprocess.run([exe,'12'],cwd=ROOT,text=True,capture_output=True,timeout=90)
    (logs/'unsigned.log').write_text(result.stdout+result.stderr)
    if result.returncode!=1 or 'FLIGHT FAIL right turn attitude sign' not in result.stderr:
        raise RuntimeError('unsigned movement divisor was not detected')
    exe=out/'clip-quad'
    command([*base,'-DR4_INJECT_FLIGHT_CLIP_QUAD=1',*sources,'-o',exe],logs/'quad.build.log')
    result=subprocess.run([exe,'12'],cwd=ROOT,text=True,capture_output=True,timeout=90)
    (logs/'quad.log').write_text(result.stdout+result.stderr)
    if result.returncode!=1 or 'FLIGHT FAIL near-clipped surface matches collision' not in result.stderr:
        raise RuntimeError('near-plane quad triangulation defect was not detected')
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',frames_per_zone=frames,widths=[640,854,1792],
        checks=['damped controls and attitude signs','fixed speed and camera cadence','world anchors and mesh planarity','near-clipped raster depth versus collision',
                'frozen uncached record and geometry-pixel CRC','cached/uncached byte equality','four zones',
                'prewarm deadline','viewport/projection invalidation','capacity fallback',
                'background all rows and padding','64/32 parity and sanitizer'],
        mutation=['EXPECTED_RED stale viewport records','EXPECTED_RED reverse world movement','EXPECTED_RED unsigned attitude divisor','EXPECTED_RED clipped quad changes mesh']))
    print('FLIGHT SOFTWARE PASS')


if __name__=='__main__':main()
