"""Record the post-deletion inventory and bind the restored console startup."""
import hashlib
import json
import shutil
import zipfile
from prepare import ROOT, BIT, OUT, RETIRED, KEEP, file_id, regular_files, write_json


def main():
    inventory = json.loads((OUT / 'inventory-before.json').read_text(encoding='utf-8'))
    deletion = json.loads((OUT / 'deletion.json').read_text(encoding='utf-8-sig'))
    history = json.loads((OUT / 'retention-verification.json').read_text(encoding='utf-8'))
    assert deletion['status'] == history['status'] == 'PASS'
    assert {entry['name'] for entry in deletion['removed']} == set(RETIRED)
    assert not any((BIT / name).exists() for name in RETIRED)
    assert {path.name for path in BIT.iterdir() if path.is_dir()} == set(KEEP)
    kept = {}
    for name in KEEP:
        folder = BIT / name
        actual = {p.relative_to(folder).as_posix(): file_id(p)
                  for p in regular_files(folder, exclude_runtime=True)}
        assert actual == inventory['retained'][name], 'protected payload changed: ' + name
        kept[name] = dict(files=len(actual), bytes=sum(v['bytes'] for v in actual.values()))

    run = json.loads((OUT / 'board/run.json').read_text(encoding='utf-8'))
    physical = json.loads((OUT / 'board/logs/r4-board/result.json').read_text(encoding='utf-8'))
    tree = ROOT / physical['b7_tree']
    snapshot = ROOT / 'Main/bit/snapshots/B7/gfx2d_ti60.bit'
    assert file_id(snapshot)['sha256'] == physical['frozen_bit_sha256']
    reports = {}
    for relative in ('config.json', 'fpga/gfx2d_ti60/outflow/gfx2d_ti60.place.rpt',
                     'fpga/gfx2d_ti60/outflow/gfx2d_ti60.timing.rpt',
                     'fpga/gfx2d_ti60/outflow/gfx2d_ti60.warn.log'):
        source = tree / relative
        identity = file_id(source)
        captured = run['artifacts'][source.relative_to(ROOT).as_posix()]
        assert identity['bytes'] == captured['size'] and identity['sha256'] == captured['sha256']
        reports[relative] = identity
    archive = OUT / 'hardware-reports.zip'
    if not archive.exists():
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zipped:
            for relative in reports:
                zipped.write(tree / relative, relative)
    with zipfile.ZipFile(archive) as zipped:
        assert zipped.testzip() is None and set(zipped.namelist()) == set(reports)
        for relative, identity in reports.items():
            body = zipped.read(relative)
            assert len(body) == identity['bytes'] and hashlib.sha256(body).hexdigest() == identity['sha256']

    package = BIT / 'board_release_console_full_20261030'
    runtime = package / 'runtime/20261030_154020'
    manifest = json.loads((package / 'manifest.json').read_text(encoding='utf-8'))
    serial = (runtime / 'serial.log').read_text(encoding='utf-8-sig')
    deploy = (runtime / 'deploy.log').read_text(encoding='utf-8-sig')
    boundary = '# --- 程序已加载，以下才是它的输出 ---'
    body = serial.split(boundary, 1)[1]
    assert 'console boot' in body and 'console enter menu' in body
    status = next(line for line in body.splitlines() if line.startswith('console status '))
    assert all(field in status for field in ('errors 0 ', 'faults 0 ', 'caps 0000d46f'))
    binary = package / manifest['board_bin']
    assert str(binary.stat().st_size) + ' bytes written at address' in deploy
    destination = OUT / 'restore'
    destination.mkdir(exist_ok=True)
    restored = {}
    for source in (runtime / 'serial.log', runtime / 'deploy.log',
                   ROOT / 'Main/sim/build/bit-retirement-restore.log'):
        target = destination / source.name
        if target.exists():
            assert file_id(target) == file_id(source), 'refusing to replace startup evidence'
        else:
            shutil.copy2(source, target)
        restored[target.relative_to(OUT).as_posix()] = file_id(target)
    historical_bytes = sum(p.stat().st_size for name in ('records', 'sources')
                           for p in (OUT / name).rglob('*') if p.is_file())
    result = dict(status='PASS', removed_bytes=deletion['removed_bytes'],
                  retired_absent=list(RETIRED), retained_static_files=kept,
                  retained_static_identity='byte-identical to inventory-before.json',
                  historical_records_and_source_deltas_bytes=historical_bytes,
                  history_verification=history,
                  hardware_reports=dict(file=archive.name, **file_id(archive), files=reports,
                                        bit=snapshot.relative_to(ROOT).as_posix(), bit_identity=file_id(snapshot),
                                        scope='existing matching B7 reports; no current-RTL synthesis'),
                  board_restore=dict(status='PASS', package=package.relative_to(ROOT).as_posix(),
                                     transport='JTAG', flash_written=False,
                                     serial_status=status, logs=restored),
                  scope='post-deletion integrity and startup; no current-RTL or full-performance acceptance')
    write_json(OUT / 'post-cleanup.json', result)
    print('POST CLEANUP PASS: seven directories absent; retained static files byte-identical; '
          'history reconstructs without retired packages; current full console running')


if __name__ == '__main__':
    main()
