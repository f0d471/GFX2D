#!/usr/bin/env python3
"""Resolve content selection into build inputs, C metadata and a manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT=Path(__file__).resolve().parents[3]
APP=ROOT/'Main/sw/apps/console'
DEFAULT=APP/'catalog.json'
sys.path.insert(0,str(ROOT/'Main/tools/check'))
from feature_config import CAPS_BIT


def load(path=DEFAULT):
    def unique(pairs):
        result={}
        for k,v in pairs:
            if k in result:raise ValueError('duplicate JSON field: '+k)
            result[k]=v
        return result
    data=json.loads(Path(path).read_text(encoding='utf-8'),object_pairs_hook=unique)
    if data.get('version')!=1:raise ValueError('unsupported catalog version')
    seen={k:set() for k in ('id','key','enum','symbol','shortcut')}
    for a in data['apps']:
        for k in ('key','enum','symbol'):
            if len(a[k])>48 or not re.fullmatch(r'[A-Za-z_]\w*',a[k],re.ASCII):raise ValueError('invalid application '+k)
        if type(a['id']) is not int or not 0<a['id']<0xffffffff:raise ValueError('invalid/reserved ID')
        if a['key'] in ('menu','fault') or type(a['host']) is not bool:raise ValueError('invalid application')
        if a['enum']=='APPFW_MENU' or a['symbol']=='g_app_menu':raise ValueError('reserved menu symbol')
        if a['graphics'] not in ('2d','3d') or a['kind'] not in ('game','demo','benchmark','system'):raise ValueError('invalid category')
        if not a['title'] or len(a['title'])>80 or any(not 32<=ord(c)<=126 for c in a['title']):raise ValueError('invalid title')
        if type(a['shortcut']) is not int or a['shortcut'] not in range(5):raise ValueError('invalid shortcut')
        for k in seen:
            if k=='shortcut' and not a[k]:continue
            if a[k] in seen[k]:raise ValueError('duplicate application '+k)
            seen[k].add(a[k])
        if set(a['target_requires'])-CAPS_BIT.keys():raise ValueError('unknown capability')
        if set(a['modules'])-data['modules'].keys():raise ValueError('unknown module')
        if a.get('describe') and not re.fullmatch(r'[A-Za-z_]\w*',a['describe'],re.ASCII):raise ValueError('invalid callback')
    for m in data['modules'].values():
        for name in [*m['defines'],*([m['boot']] if m.get('boot') else [])]:
            if not re.fullmatch(r'[A-Za-z_]\w*',name,re.ASCII):raise ValueError('invalid module symbol')
        for pattern in m['sources']:
            if any(c in pattern for c in '\n\r\t $;#\\'):raise ValueError('unsafe source pattern')
            matches=sorted(APP.glob(pattern))
            if not matches or any(not p.is_file() or not p.resolve().is_relative_to(ROOT/'Main') for p in matches):raise ValueError('invalid/missing source: '+pattern)
        for source in m['libraries']:
            p=ROOT/'Main/sw/lib'/source
            if not p.resolve().is_relative_to(ROOT/'Main/sw/lib') or not p.is_file():raise ValueError('invalid library')
    for entries in [*data['profiles'].values(),data['system']]:
        if len(entries)!=len(set(entries)) or set(entries)-seen['key']:raise ValueError('invalid profile list')
    if any(a['kind']!='system' for a in data['apps'] if a['key'] in data['system']):raise ValueError('invalid system list')
    if set(data.get('base_modules',[]))-data['modules'].keys():raise ValueError('unknown base module')
    if any(a['graphics']=='3d' for a in data['apps'] if a['key'] in data['profiles'].get('2d',[])):
        raise ValueError('2d profile contains 3D content')
    return data


def resolve(data,profile='full',apps=None):
    if profile not in data['profiles']:raise ValueError('unknown profile: '+profile)
    chosen=list(data['profiles'][profile] if apps is None else apps)
    if len(chosen)!=len(set(chosen)) or set(chosen)-{a['key'] for a in data['apps']}:raise ValueError('duplicate/unknown selected application')
    if set(chosen)&set(data['system']):raise ValueError('system applications are automatic')
    chosen+=data['system']
    entries=[next(a for a in data['apps'] if a['key']==k) for k in chosen]
    modules=sorted({m for a in entries for m in a['modules']}|set(data.get('base_modules',[])))
    payload=dict(version=1,profile=profile if apps is None else 'custom',apps=entries,modules={m:data['modules'][m] for m in modules},capability_bits=CAPS_BIT)
    fingerprint=hashlib.sha256(json.dumps(payload,sort_keys=True).encode()).hexdigest()
    return {**payload,'catalog_sha256':fingerprint,'content_ids':[a['id'] for a in entries if a['kind']!='system'],
            'enable_3d':any(a['graphics']=='3d' for a in entries),'asset_limit_bytes':4*1024*1024,'image_limit_bytes':512*1024}


def write(path,body):
    if path.exists() and path.read_text(encoding='utf-8')==body:return
    path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(body,encoding='utf-8',newline='\n');temp.replace(path)


def validate_selection(content):
    payload={k:content[k] for k in ('version','profile','apps','modules','capability_bits')}
    if content['version']!=1 or content['catalog_sha256']!=hashlib.sha256(json.dumps(payload,sort_keys=True).encode()).hexdigest():
        raise ValueError('content manifest fingerprint mismatch')
    if content['content_ids']!=[a['id'] for a in content['apps'] if a['kind']!='system']:
        raise ValueError('content ID list mismatch')
    if content['enable_3d']!=any(a['graphics']=='3d' for a in content['apps']):raise ValueError('content category mismatch')
    if content['image_limit_bytes']!=512*1024 or content['asset_limit_bytes']!=4*1024*1024:
        raise ValueError('content budget mismatch')
    return content


def generate(data,selection,out):
    if not out.resolve().is_relative_to(ROOT):raise ValueError('output must stay inside checkout')
    all_defines={d for m in data['modules'].values() for d in m['defines']}
    enabled={d for m in selection['modules'].values() for d in m['defines']}
    flags=['-D'+d+'='+str(int(d in enabled)) for d in sorted(all_defines)]
    flags+=['-DCONSOLE_ENABLE_3D='+str(int(selection['enable_3d'])),'-DCONSOLE_CATALOG_TAG=0x'+selection['catalog_sha256'][:8]+'u']
    sources=sorted({os.path.relpath(p,APP).replace('\\','/') for m in selection['modules'].values() for pat in m['sources'] for p in APP.glob(pat)})
    all_libs={p for m in data['modules'].values() for p in m['libraries']}
    selected_libs={p for m in selection['modules'].values() for p in m['libraries']}
    write(out/'selection.mk','CONSOLE_APP_SRCS := '+' '.join(sources)+'\nCONSOLE_FLAGS := '+' '.join(flags)+'\nLIB_EXCLUDE := '+' '.join(sorted(all_libs-selected_libs))+'\n')
    ids='#ifndef CONSOLE_CATALOG_IDS_H\n#define CONSOLE_CATALOG_IDS_H\n#define APPFW_MENU 0u\n'
    for a in data['apps']:
        ids+='#define '+a['enum']+' '+str(a['id'])+'u\n'
        ids+='#define CONSOLE_KEY_'+a['enum']+' '+json.dumps(a['key'])+'\n#define CONSOLE_TITLE_'+a['enum']+' '+json.dumps(a['title'])+'\n'
    write(out/'console_catalog_ids.h',ids+'#endif\n')
    inc='/* Generated from catalog.json. Only selected applications are referenced. */\n'
    for a in selection['apps']:
        inc+='extern const appfw_app '+a['symbol']+';\n'
        if a.get('describe'):inc+='extern void '+a['describe']+'(appfw_ctx *,appfw_detail *);\n'
    hooks=sorted({m['boot'] for m in selection['modules'].values() if m.get('boot')})
    for h in hooks:inc+='extern void '+h+'(appfw_ctx *);\n'
    inc+='const appfw_app *const g_appfw_registry[]={\n'+''.join(' &'+a['symbol']+',\n' for a in selection['apps'])+'};\n'
    inc+='const uint32_t g_appfw_registry_n=sizeof g_appfw_registry/sizeof g_appfw_registry[0];\nconst console_app_meta g_console_catalog[]={\n'
    for a in selection['apps']:
        caps=sum(1<<CAPS_BIT[x] for x in set(a['target_requires']))
        inc+=' {&%s,0x%08xu,%du,%du,%s,%s,%s},\n'%(a['symbol'],caps,a['host'],a['shortcut'],json.dumps(a['kind']),json.dumps(a['graphics']),a.get('describe','NULL'))
    inc+='};\nconst char g_console_profile[]='+json.dumps(selection['profile'])+';\nconst char g_console_catalog_sha256[]='+json.dumps(selection['catalog_sha256'])+';\n'
    inc+='void console_modules_boot(appfw_ctx *ctx) { (void)ctx;'+''.join(h+'(ctx);' for h in hooks)+' }\n'
    write(out/'console_catalog.inc',inc)
    write(out/'content.json',json.dumps(selection,indent=2,sort_keys=True)+'\n')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--catalog',type=Path,default=DEFAULT);ap.add_argument('--profile',default='full')
    ap.add_argument('--apps',help='comma/space separated content keys; empty selects system only')
    ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
    try:
        data=load(a.catalog);selection=resolve(data,a.profile,None if a.apps is None else a.apps.replace(',',' ').split());generate(data,selection,a.out)
    except (ValueError,KeyError,TypeError,OSError) as exc:ap.exit(2,'CATALOG ERROR: '+str(exc)+'\n')
    print('CATALOG_OK')


if __name__=='__main__':main()
