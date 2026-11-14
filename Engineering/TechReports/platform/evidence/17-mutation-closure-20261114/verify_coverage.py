"""只读复核本工作点的 79 条覆盖；保留不同运行的来源，不伪造单轮汇总。"""
from pathlib import Path
import copy
import hashlib
import json
import sys

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[4]
sys.path[:0] = [str(ROOT/'Main/tools/check'), str(ROOT/'Main/sim/mutation')]
import yaml
import run_mutation as campaign
from check_run import input_identity


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def frozen_bytes(name):
    """逆向还原已单独证明的注释整理，要求字节摘要、行数和可执行前缀相符。"""
    data = (ROOT/name).read_bytes()
    for record in read(BASE/'comment-review.json')['files']:
        if record['path'] != name:
            continue
        assert digest(data) == record['after_sha256'], name
        lines = data.decode().splitlines(keepends=True)
        for change in record['comment_changes']:
            i = change['line']-1
            assert lines[i].rstrip('\r\n') == change['after'], name
            ending = lines[i][len(lines[i].rstrip('\r\n')):]
            a, b = change['before'], change['after']
            marker = '//' if '//' in a else '#'
            assert a.split(marker, 1)[0] == b.split(marker, 1)[0], name
            lines[i] = a+ending
        data = ''.join(lines).encode()
        assert digest(data) == record['before_sha256'], name
    return data


def main():
    old = BASE/'full-diagnostic'
    new = BASE/'affected-final'
    a = read(old/'logs/mutation/mutation-report.json')
    b = read(new/'logs/mutation/mutation-report.json')
    rows = campaign.load_manifest()
    before = {x['id']: x for x in a['mutants']}
    after = {x['id']: x for x in b['mutants']}
    affected = {r['id'] for r in rows if any(x['cmd'].startswith('bash Main/sim/engine/mover/run.sh') for x in r['runs'])}
    assert len(affected) == 7
    assert set(after) == affected | {'geom.band_clip', 'rast.rast_partial_commit'}
    assert a['profile'] == b['profile'] == 'acceptance'
    assert len(a['baseline']) == 28 and all(x['status'] == 'PASS' for x in a['baseline'].values())
    assert b['status'] == 'PASS' and all(x['status'] == 'PASS' for x in b['baseline'].values())
    assert set(before) == {r['id'] for r in rows} and len(rows) == 79
    snapshot = read(old/'logs/mutation/snapshot.json')['inputs']
    changed = {n for n, v in snapshot.items() if digest(frozen_bytes(n)) != v['sha256']}
    assert changed == {'Main/sim/engine/mover/tb_engine_mover.v', 'Main/sim/mutation/mutants.yaml', 'Main/tools/check/validate.py'}, changed
    name = 'Main/sim/engine/mover/tb_engine_mover.v'
    text = frozen_bytes(name).decode()
    start = text.index('    // 从请求字节数独立对账 DMA 输入流')
    end = text.index('    always @(posedge clk) if (!rst)', start)
    assert digest((text[:start]+text[end:]).encode()) == snapshot[name]['sha256']
    previous = copy.deepcopy(rows)
    for row in previous:
        original = before[row['id']]
        assert row['target'] == original['target'] and row['rules'] == original['rules']
        assert [r['cmd'] for r in row['runs']] == [r['cmd'] for r in original['runs']]
        for run, saved in zip(row['runs'], original['runs']):
            run['evidence'] = saved['expected_evidence']
    manifest = ROOT/'Main/sim/mutation/mutants.yaml'
    header = manifest.read_text().split('- id:', 1)[0]
    assert digest((header+yaml.safe_dump(previous, allow_unicode=True, sort_keys=False, width=10000)).encode()) == snapshot['Main/sim/mutation/mutants.yaml']['sha256']
    text = frozen_bytes('Main/tools/check/validate.py').decode()
    text = text.replace("    parser.add_argument('--mutation-ids',nargs='+',help='按影响选择 mutation 条目；命令和范围写入计划')\n", '')
    start = text.index('        if args.mutation_ids:')
    end = text.index("        CHECKS['mutation']['inputs']", start)
    text = text[:start]+text[end:]
    text = text.replace("    elif args.mutation_ids:parser.error('--mutation-ids 需要选择 mutation')\n", '')
    assert digest(text.encode()) == snapshot['Main/tools/check/validate.py']['sha256']
    references = [n for n in snapshot if Path(n).suffix in ('.py', '.sh') and b'tb_engine_mover.v' in frozen_bytes(n)]
    assert references == ['Main/sim/engine/mover/run.sh']
    inputs = read(old/'run.json')['inputs']
    for row in rows:
        name = row['spec']['doc']
        assert input_identity(ROOT/name)['sha256'] == inputs[name]['sha256']
    assert read(old/'logs/mutation/toolchain.json') == read(new/'logs/mutation/toolchain.json')
    for row in rows:
        entry = (after if row['id'] in after else before)[row['id']]
        origin = new if row['id'] in after else old
        assert entry['status'] == 'KILLED'
        assert digest(campaign.mutated_bytes((ROOT/row['target']).read_bytes(), row['rules'])) == entry['source_sha256']
        for i, observer in enumerate(row['runs']):
            folder = origin/'logs/mutation/mutants'/row['id']/str(i)
            process = read(folder/'process.json')
            text = (folder/'stdout.log').read_text(errors='replace')
            for label in ('suite', 'legacy'):
                if (folder/label).exists():
                    text += '\n'+'\n'.join(p.read_text(errors='replace') for p in sorted((folder/label).rglob('*.log')))
            assert campaign.classify({**process, 'text': text}, observer['evidence']) == 'KILLED', row['id']
    print('COVERAGE PASS 79/79: unaffected frozen scope 70; rerun affected scope 9')


if __name__ == '__main__':
    main()
