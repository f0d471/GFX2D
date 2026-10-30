"""Bind the two actual check runs to the immutable deletion gate."""
import hashlib,json
from pathlib import Path
from prepare import ROOT,OUT

host=ROOT/'Main/sim/build/check-runs/20261030-030038-d6a97183'
board=ROOT/'Main/sim/build/check-runs/20261030-030035-e3284c97'
host_summary=json.loads((host/'result.json').read_text())
board_summary=json.loads((board/'result.json').read_text())
unit=json.loads((host/'logs/frozen-baseline/result.json').read_text())
physical=json.loads((board/'logs/r4-board/result.json').read_text())
assert host_summary['status']==board_summary['status']==unit['status']==physical['status']=='PASS'
bit=ROOT/'Main/bit/snapshots/B7/gfx2d_ti60.bit';digest=hashlib.sha256(bit.read_bytes()).hexdigest()
assert unit['frozen_bit_sha256']==physical['frozen_bit_sha256']==digest
assert physical['frozen_release'].replace('\\','/')=='Main/bit/snapshots/B7'
data=dict(status='PASS',bit_sha256=digest,
    host=dict(status='PASS',run=host.name,profile=host_summary['profile'],checks=host_summary['checks'],unit_cases=unit['unit_cases']),
    board=dict(status='PASS',run=board.name,profile=board_summary['profile'],tree=physical['b7_tree'],
        results=physical['results'],scope=physical['scope']),flash_written=False,current_rtl_acceptance=False)
(OUT/'validation.json').write_text(json.dumps(data,indent=2)+'\n')
print('MIGRATION VERIFIED: '+digest+'; host and physical quick PASS')
