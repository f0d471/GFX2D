#!/usr/bin/env python3
"""Run the structural mutation suite in a private source copy."""
import os
import shutil
from check_run import ROOT,session,command,write_json


def ignore(directory,names):
    return [n for n in names if n in ('.git','__pycache__','logs','.work') or n=='build' or n.startswith(('build_','build-'))]


def main():
    current=session();logs=current.logs('layers');tree=current.build('layers')/'tree'
    tree.mkdir(parents=True,exist_ok=False)
    for name in ('sw','sdk','assets','tools'):
        shutil.copytree(ROOT/'Main'/name,tree/'Main'/name,ignore=ignore)
    # Child collectors belong to the copied tree, not the frozen parent run.
    env={k:v for k,v in os.environ.items() if not k.startswith('GFX_CHECK_')}
    env['GFX_CHECK_PROFILE']=current.profile
    body=command(['bash',tree/'Main/tools/check/check_sw_layers.sh'],logs/'isolated.log',cwd=tree,env=env,timeout=2400)
    if 'fail 0' not in body or 'done' not in body:raise RuntimeError('isolated structural checks did not complete')
    current.verify();write_json(logs/'result.json',dict(status='PASS',profile=current.profile,
        scope='real host/target structural and object-isolation checks in a private source copy',production_mutations=False))
    print('LAYERS PASS isolated source and real cross builds')


if __name__=='__main__':main()
