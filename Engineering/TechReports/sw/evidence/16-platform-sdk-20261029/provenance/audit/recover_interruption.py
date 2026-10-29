"""Finalize the stale RUNNING marker of our explicitly interrupted attempt.
Never changes a check to PASS; original manifest remains byte-preserved in logs.
"""
from pathlib import Path
import json,sys
R=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(R/'Main/tools/check'))
from check_run import write_json
path=R/'Main/sim/build/check-runs/20261029-053351-7edc48c2'
data=json.loads((path/'run.json').read_text())
running=[key for key,value in data['checks'].items() if value['status']=='RUNNING']
if running:
    if running!=['check:demo']:raise RuntimeError('Unexpected active marker: '+str(running))
    logs=path/'logs/interruption';logs.mkdir(exist_ok=True)
    saved=logs/'run-before-recovery.json'
    if saved.exists():raise RuntimeError('Original interruption manifest already saved')
    saved.write_bytes((path/'run.json').read_bytes())
    entry=data['checks']['check:demo']
    entry.update(status='INCOMPLETE',error='Owner explicitly stopped this attempt after missing toolchain configuration; no PASS claimed',
        interrupted=True,original_marker='RUNNING')
    write_json(path/'run.json',data)
    write_json(logs/'recovery.json',dict(action='RUNNING to INCOMPLETE',check='check:demo',
        reason='explicit owner interruption; process terminated in this task',pass_statuses_unchanged=True))
print('Interrupted attempt recorded as INCOMPLETE; original manifest retained')
