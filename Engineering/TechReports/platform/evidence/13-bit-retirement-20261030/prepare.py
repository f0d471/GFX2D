"""Inventory and retain historical records before the explicitly scoped deletion.

No executable/bitstream is copied. Old source snapshots are reduced to Git-based
content deltas and verified by reconstructing every original ZIP entry.
"""
from io import BytesIO
from pathlib import Path,PurePosixPath
import hashlib,json,os,shutil,subprocess,zipfile

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'Main/tools/check/validate.py').is_file())
BIT=ROOT/'Main/bit';OUT=Path(__file__).parent
RETIRED=('board_release_20261015','board_release_console_20261028',
 'board_release_game_current_20261024','board_release_game_next_20261024',
 'board_release_game_wide_20261025','board_release_r3_20261023','board_release_r4_visual_20261024')
KEEP=('snapshots','official','flash_backup','board_release_console_2d_20261030','board_release_console_full_20261030')
def sha(data):return hashlib.sha256(data).hexdigest()
def file_id(p):
 data=p.read_bytes();return dict(bytes=len(data),sha256=sha(data))
def regular_files(folder,exclude_runtime=False):
 assert folder.resolve().parent==BIT.resolve() and folder.name in RETIRED+KEEP
 paths=[]
 for base,dirs,names in os.walk(folder,followlinks=False):
  for name in dirs+names:
   p=Path(base)/name
   if p.is_symlink() or getattr(p.lstat(),'st_file_attributes',0)&0x400:raise RuntimeError('reparse point: '+str(p))
  for name in names:
   p=Path(base)/name
   if exclude_runtime and p.relative_to(folder).parts[0]=='runtime':continue
   paths.append(p)
 return sorted(paths)
def write_json(p,data):p.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def git(*args):return subprocess.check_output(['git','-c','core.quotepath=false',*args],cwd=ROOT,stderr=subprocess.PIPE)
def git_blob(data):return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
def git_blobs(ids):
 ids=sorted(set(ids))
 if not ids:return {}
 raw=subprocess.check_output(
     ['git','cat-file','--batch'],input=('\n'.join(ids)+'\n').encode(),cwd=ROOT,stderr=subprocess.PIPE)
 stream=BytesIO(raw);result={}
 for expected in ids:
  header=stream.readline().decode().split();assert header[:2]==[expected,'blob']
  body=stream.read(int(header[2]));assert stream.read(1)==b'\n' and git_blob(body)==expected
  result[expected]=body
 return result

def source_delta(folder,head):
 original=folder/'source.zip';assert len(head)==40
 tree={}
 for line in git('ls-tree','-r','-z',head,'--','Main').split(b'\0'):
  if line:
   meta,name=line.split(b'\t',1);mode,kind,oid=meta.decode().split()
   if kind=='blob':tree[name.decode('utf-8')]=oid
 index=[];changed={}
 with zipfile.ZipFile(original) as z:
  assert z.testzip() is None
  for item in z.infolist():
   if item.is_dir():continue
   name=item.filename;p=PurePosixPath(name)
   assert not p.is_absolute() and '..' not in p.parts and p.parts[0]=='Main'
   data=z.read(item);oid=tree.get(name);entry=dict(path=name,bytes=len(data),sha256=sha(data))
   if oid and git_blob(data)==oid:entry.update(baseline_blob=oid,transform='none')
   elif oid and b'\r\n' in data and data.replace(b'\r\n',b'').find(b'\n')<0 and git_blob(data.replace(b'\r\n',b'\n'))==oid:
    entry.update(baseline_blob=oid,transform='lf_to_crlf')
   else:entry['delta']='files/'+name;changed[entry['delta']]=data
   index.append(entry)
 payload=dict(version=1,source_head=head,original_zip=file_id(original),entries=index,
     scope='exact selected source file contents; historical executables/bitstreams are retired')
 destination=OUT/'sources'/(folder.name+'.delta.zip');destination.parent.mkdir(exist_ok=True)
 if destination.exists():raise RuntimeError('refusing to replace historical source delta')
 with zipfile.ZipFile(destination,'w',zipfile.ZIP_DEFLATED) as z:
  z.writestr('index.json',json.dumps(payload,ensure_ascii=False,indent=2)+'\n')
  for name,data in changed.items():z.writestr(name,data)
 blobs=git_blobs(e['baseline_blob'] for e in index if 'baseline_blob' in e)
 with zipfile.ZipFile(destination) as z:
  assert z.testzip() is None
  for e in index:
   data=z.read(e['delta']) if 'delta' in e else blobs[e['baseline_blob']]
   if e.get('transform')=='lf_to_crlf':data=data.replace(b'\n',b'\r\n')
   assert len(data)==e['bytes'] and sha(data)==e['sha256'],'source reconstruction failed: '+e['path']
 return dict(file=destination.relative_to(OUT).as_posix(),**file_id(destination),
     source_head=head,entries=len(index),delta_entries=len(changed),reconstruction='PASS',original_zip=file_id(original))

def main():
 baseline=OUT/'inventory-before.json'
 if baseline.exists():raise RuntimeError('retirement was already inventoried; do not overwrite it')
 inventory=dict(version=1,retired={},retained={},runtime_files='mutable runtime files of retained packages excluded from content comparison')
 for name in RETIRED:
  folder=BIT/name;inventory['retired'][name]={p.relative_to(folder).as_posix():file_id(p) for p in regular_files(folder)}
 for name in KEEP:
  folder=BIT/name;inventory['retained'][name]={p.relative_to(folder).as_posix():file_id(p) for p in regular_files(folder,True)}
 write_json(baseline,inventory)
 results={};stored={}
 for name in RETIRED:
  folder=BIT/name;records=[]
  for p in regular_files(folder):
   relative=p.relative_to(folder);n=p.name.lower()
   retain=(n in ('readme.md','identity.json','verification.json','source.diff') or
       (n.startswith('manifest') and p.suffix=='.json') or p.suffix=='.log' or
       (relative.parts[0]=='qa' and p.suffix in ('.png','.json')))
   if not retain:continue
   identity=file_id(p)
   canonical=ROOT/'Main/sw/apps/game/gfx2dGame/logs/board_game_hw_ddr_20261015.log'
   if p.name=='board_game_hw_ddr_20261015.log' and canonical.is_file() and file_id(canonical)==identity:
    records.append(dict(source=relative.as_posix(),canonical=canonical.relative_to(ROOT).as_posix(),**identity));continue
   if identity['sha256'] not in stored:
    target=OUT/'records'/name/relative;target.parent.mkdir(parents=True,exist_ok=True)
    if target.exists():raise RuntimeError('archive record exists')
    shutil.copy2(p,target);assert file_id(target)==identity
    stored[identity['sha256']]=target.relative_to(OUT).as_posix()
   records.append(dict(source=relative.as_posix(),stored=stored[identity['sha256']],**identity))
  result=dict(original_bytes=sum(v['bytes'] for v in inventory['retired'][name].values()),records=records)
  if (folder/'source.zip').is_file():
   m=json.loads((folder/'manifest.json').read_text(encoding='utf-8-sig'))
   result['source_delta']=source_delta(folder,m['source_head'])
  results[name]=result
 write_json(OUT/'retention.json',dict(status='PREPARED',records_verified=True,source_reconstruction_verified=True,packages=results))
 print('HISTORY PREPARED: '+str(sum(v['original_bytes'] for v in results.values()))+' original bytes; '+str(len(stored))+' unique records')
 for name,v in results.items():
  if 'source_delta' in v:print(name+': '+str(v['source_delta']['delta_entries'])+' changed/new files, delta '+str(v['source_delta']['bytes'])+' bytes')

if __name__=='__main__':main()
