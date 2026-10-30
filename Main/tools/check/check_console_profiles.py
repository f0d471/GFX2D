#!/usr/bin/env python3
"""Selected-content builds, real menus, arbitrary IDs and negative catalogs."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
from check_run import ROOT,command,session,write_json
sys.path.insert(0,str(ROOT/'Main/tools/board'))
from console_catalog import load,resolve,validate_selection,APP
from console_checks import validate_host,catalog


def require(value,message):
    if not value:raise RuntimeError(message)


def main():
    current=session();logs=current.logs('console-profiles');build=current.build('console-profiles')
    data=load()
    require(resolve(data,'2d')['content_ids']==[1,2,3],'2D release selection changed')
    require(resolve(data,'full')['content_ids']==[1,2,3,4],'full release selection changed')
    for field,value in [('catalog_sha256','f'*64),('enable_3d',True),('content_ids',[1,2]),('image_limit_bytes',9999999)]:
        mutated=resolve(data,'2d');mutated[field]=value
        try:validate_selection(mutated)
        except ValueError:pass
        else:raise RuntimeError('modified content manifest accepted: '+field)
    bad=[]
    for field in ('id','key','enum','symbol','shortcut'):
        mutant=copy.deepcopy(data);mutant['apps'][1][field]=mutant['apps'][0][field];bad.append(mutant)
    for field,value in [('id',0),('id',0xffffffff),('host',1),('graphics','4d'),('target_requires',['MAGIC']),('modules',['missing'])]:
        mutant=copy.deepcopy(data);mutant['apps'][0][field]=value;bad.append(mutant)
    mutant=copy.deepcopy(data);mutant['profiles']['2d'].append('r4');bad.append(mutant)
    for i,mutant in enumerate(bad):
        path=build/f'bad-{i}.json';path.write_text(json.dumps(mutant))
        try:load(path)
        except ValueError:pass
        else:raise RuntimeError('invalid catalog accepted: '+str(i))
    for selected in [['missing'],['demo','demo'],['about']]:
        try:resolve(data,apps=selected)
        except ValueError:pass
        else:raise RuntimeError('invalid selection accepted')
    includes=['-I'+str(ROOT/'Main/sw/runtime/include'),'-I'+str(ROOT/'Main/sw/ports/myriad')]
    for label,flags in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        exe=build/('menu-'+label)
        command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',*flags,*includes,
                 APP/'test/test_menu_cursor.c',ROOT/'Main/sw/runtime/menu_cursor.c','-o',exe],logs/('menu-'+label+'.build.log'))
        command([exe],logs/('menu-'+label+'.log'))
    env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1'}
    script='5:128,6:0,20:256,21:0,35:512,36:0,50:896,51:0,70:2048,71:0'
    for profile in ('2d','full'):
        out=build/('host-'+profile)
        command(['make','-C',APP,'-j4','PLATFORM=host','PERF=1','APP_PROFILE='+profile,'OBJDIR='+str(out)],logs/(profile+'.build.log'))
        expected=json.loads((out/'catalog/content.json').read_text())
        body=command([out/'console-perf','--frames','80','--input-script',script],logs/(profile+'.log'),env=env)
        validate_host(body,expected)
        names=subprocess.check_output(['nm','--defined-only',str(out/'console-perf')],text=True)
        if profile=='2d':
            require(' g_app_r4' not in names and not any(line.split()[-1].startswith(('r4_','rast_','replay_r4_')) for line in names.splitlines()),'2D binary retained 3D code')
            result=subprocess.run([str(out/'console-perf'),'--app','r4'],cwd=ROOT,env=env,capture_output=True,text=True)
            require(result.returncode==2 and 'Unknown application' in result.stderr,'CLI bypassed selection')
            for broken in [body.replace('count 4','count 5'),body.replace('available 1','available 0',1),body.replace('catalog end','catalog truncated')]:
                try:catalog(broken,expected)
                except RuntimeError:pass
                else:raise RuntimeError('collector accepted broken catalog')
        command([out/'console-perf','--app','stg','--frames','3'],logs/(profile+'-by-name.log'),env=env)
    # Reuse one object directory across A/B/A content selections. A changed source
    # set must not retain a formerly linked R4 or stale generated registry.
    out=build/'switch'
    for n,profile in enumerate(('2d','full','2d')):
        command(['make','-s','-C',APP,'-j4','PLATFORM=host','PERF=1','APP_PROFILE='+profile,'OBJDIR='+str(out)],logs/(f'switch-{n}.build.log'))
        body=command([out/'console-perf','--list-apps'],logs/(f'switch-{n}.log'))
        require(('app 4 r4' in body)==(profile=='full'),'stale registry after content switch')
    # A real generated build with twenty extra entries and sparse IDs, including
    # multiple 3D labels that have no R4 dependency. Exercise scrolling and launch.
    extra=copy.deepcopy(data);fixture=build/'extra.c'
    fixture.write_text('#include "console_views.h"\n#include "app_port.h"\n'
        'static int enter(appfw_ctx*c){return console_view_prepare(c,CONSOLE_VIEW_STG,0);}\n'
        'static int leave(appfw_ctx*c){return console_view_flush(c);}\n'
        'static int frame(appfw_ctx*c,uint32_t k,uint32_t n){gfx_list l;int d;(void)k;(void)n;'
        'if(console_view_begin(c,&l,&d))return -1;gfx_rect r={0,0,639,479};'
        'if(gfx_set_clip(&l,r)||gfx_fill(&l,d,r,0x123456,GFX_EQ_COPY,0)||gfx_list_end(&l))return -1;'
        'return console_view_submit(c,&l,d,0);}\n'+
        ''.join('const appfw_app g_extra%d={%d,"extra%d","EXTRA %d",enter,leave,frame};\n'%(i,42+i*7,i,i) for i in range(20)))
    for i in range(20):
        extra['apps'].append(dict(key='extra'+str(i),id=42+i*7,enum='APP_EXTRA'+str(i),symbol='g_extra'+str(i),title='EXTRA '+str(i),kind='game',graphics='3d' if i%2 else '2d',modules=['extra'],target_requires=[],host=True,shortcut=0))
    extra['modules']['extra']=dict(sources=[os.path.relpath(fixture,APP).replace('\\','/')],libraries=[],defines=[])
    path=build/'extra.json';path.write_text(json.dumps(extra));load(path)
    selected=['extra'+str(i) for i in range(20)];out=build/'extra-host'
    command(['make','-C',APP,'-j4','PLATFORM=host','PERF=1','OBJDIR='+str(out),'APP_CATALOG='+str(path),'APPS='+' '.join(selected)],logs/'extra.build.log')
    expected=json.loads((out/'catalog/content.json').read_text())
    events=[]
    for i in range(13):events.extend([(2+i*2,2),(3+i*2,0)])
    events.extend([(30,1024),(31,0)])
    body=command([out/'console-perf','--frames','45','--input-script',','.join(f'{t}:{k}' for t,k in events)],logs/'extra-menu.log',env=env)
    catalog(body,expected);require('console enter extra13' in body and 'errors 0' in body,'scroll did not launch filtered item 13')
    body=command([out/'console-perf','--app','42','--frames','4'],logs/'sparse-id.log',env=env)
    require('console enter extra0' in body,'CLI rejected sparse ID')
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,invalid_catalogs=len(bad),extra_apps=20,
        checks=['2d/full builds','symbol pruning','A/B/A','menu 0/1/3/multipage','capability alternatives','sparse IDs','catalog negatives']))
    print('CONSOLE PROFILES PASS selection/pruning/multipage/sparse-ID/capabilities/A-B-A')


if __name__=='__main__':main()
