"""Verify retained history without reading any of the retired directories."""
import json,zipfile
from prepare import ROOT,OUT,sha,file_id,git_blobs

def main():
    retention=json.loads((OUT/'retention.json').read_text(encoding='utf-8'))
    records=source_files=0
    for name,package in retention['packages'].items():
        for record in package['records']:
            path=ROOT/record['canonical'] if 'canonical' in record else OUT/record['stored']
            assert file_id(path)=={k:record[k] for k in ('bytes','sha256')},'retained record differs'
            records+=1
        if 'source_delta' not in package:continue
        archive=OUT/package['source_delta']['file']
        assert file_id(archive)=={k:package['source_delta'][k] for k in ('bytes','sha256')}
        with zipfile.ZipFile(archive) as z:
            assert z.testzip() is None
            index=json.loads(z.read('index.json'))
            blobs=git_blobs(e['baseline_blob'] for e in index['entries'] if 'baseline_blob' in e)
            for entry in index['entries']:
                body=z.read(entry['delta']) if 'delta' in entry else blobs[entry['baseline_blob']]
                if entry.get('transform')=='lf_to_crlf':body=body.replace(b'\n',b'\r\n')
                assert len(body)==entry['bytes'] and sha(body)==entry['sha256'],'source content differs'
                source_files+=1
    result=dict(status='PASS',retired_directories_read=False,record_references=records,restored_source_files=source_files)
    (OUT/'retention-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print('RETAINED HISTORY PASS: '+str(records)+' record references, '+str(source_files)+' reconstructed source files; no retired directory read')

if __name__=='__main__':main()
