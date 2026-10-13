#!/usr/bin/env python3
"""本次取证包的归整与 CRC／日志指纹复核。只处理同目录已登记的运行快照。"""
import hashlib
import json
from pathlib import Path
import zipfile

BASE=Path(__file__).resolve().parent


def main():
    index={}
    for folder in sorted(BASE.iterdir()):
        if not folder.is_dir() or not (folder/'archive.json').exists():continue
        target=folder/'records.zip'
        manifest=json.loads((folder/'archive-files.json').read_text(encoding='utf-8'))
        if target.exists():
            with zipfile.ZipFile(target) as archive:
                for name,expected in manifest.items():
                    if hashlib.sha256(archive.read(name)).hexdigest()!=expected['sha256']:
                        raise RuntimeError('日志身份不符: '+folder.name+'/'+name)
            print('VERIFY '+folder.name)
        else:
            keep={'archive.json','archive-files.json'}
            if folder.name=='host-rtl':keep.add('run.json')
            files=[p for p in sorted(folder.rglob('*')) if p.is_file() and p.relative_to(folder).as_posix() not in keep]
            if any(p.is_symlink() or not p.resolve().is_relative_to(folder.resolve()) for p in files):
                raise RuntimeError('快照包含链接或越界路径')
            payload={p.relative_to(folder).as_posix():p.read_bytes() for p in files}
            data=json.loads((folder/'run.json').read_text(encoding='utf-8'))
            summary={'run_id':data['run_id'],'revision':data['revision'],'profile':data['profile'],
                     'checks':{name:item['status'] for name,item in data['checks'].items()},
                     'source_snapshot':'records.zip/run.json' if folder.name!='host-rtl' else 'run.json',
                     'scope':'historical snapshot; no new current-source acceptance'}
            pending=folder/'records.zip.pending'
            with zipfile.ZipFile(pending,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
                for name,body in payload.items():archive.writestr(name,body)
            with zipfile.ZipFile(pending) as archive:
                for name,body in payload.items():
                    restored=archive.read(name)  # 解压同时检查 CRC。
                    if restored!=body:raise RuntimeError('压缩回读不一致: '+name)
                    if name in manifest and hashlib.sha256(restored).hexdigest()!=manifest[name]['sha256']:
                        raise RuntimeError('原始日志已改变: '+name)
            pending.replace(target)
            (folder/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            # 已逐项回读成功后只删已枚举文件；空目录逐个移除，不递归跟随链接。
            for p in files:p.unlink()
            for p in sorted((p for p in folder.rglob('*') if p.is_dir()),key=lambda p:len(p.parts),reverse=True):
                if p.is_symlink():raise RuntimeError('拒绝处理链接目录')
                if not any(p.iterdir()):p.rmdir()
            print('PACK '+folder.name+' files='+str(len(payload)))
        index[folder.name]={'archive':folder.name+'/records.zip','size':target.stat().st_size,
                            'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),
                            'log_manifest':folder.name+'/archive-files.json'}
    (BASE/'archives.json').write_text(json.dumps(index,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('PACK PASS archives='+str(len(index)))


if __name__=='__main__':main()
