#!/usr/bin/env python3
"""Frozen B7 identity formats and tree selection, without programming hardware."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from check_run import ROOT,session,write_json
import check_r4_board as subject


class FrozenIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=TEST_BUILD)
        self.base=Path(self.temp.name);self.payload=b'fixed B7 test fixture'
        assert self.base.resolve().is_relative_to(TEST_BUILD.resolve())
        self.digest=hashlib.sha256(self.payload).hexdigest()
    def tearDown(self):self.temp.cleanup()
    def snapshot(self,folder=None):
        folder=folder or self.base/'snapshot';folder.mkdir(parents=True,exist_ok=True)
        (folder/'gfx2d_ti60.bit').write_bytes(self.payload)
        data=dict(config='B7',caps='0x0000D46F',bit_sha256=self.digest,bit_bytes=len(self.payload))
        self.save(folder,data);return folder,data
    def save(self,folder,data):(folder/'manifest.json').write_text(json.dumps(data),encoding='utf-8')
    def reject(self,data):
        folder,_=self.snapshot();self.save(folder,data)
        with self.assertRaises(RuntimeError):subject.frozen_bit_identity(folder)
    def test_snapshot_identity(self):
        folder,_=self.snapshot();digest,manifest=subject.frozen_bit_identity(folder)
        self.assertEqual(digest,self.digest);self.assertEqual(manifest,folder/'manifest.json')
    def test_historical_package(self):
        folder=self.base/'legacy';(folder/'board').mkdir(parents=True)
        (folder/'board/B7.bit').write_bytes(self.payload)
        self.save(folder,dict(files=[dict(path='board/B7.bit',sha256=self.digest)]))
        self.assertEqual(subject.frozen_bit_identity(folder)[0],self.digest)
    def test_generic_product_package(self):
        folder=self.base/'product';(folder/'board').mkdir(parents=True)
        (folder/'board/gfx2d_ti60.bit').write_bytes(self.payload)
        self.save(folder,dict(board_bit='board/gfx2d_ti60.bit',bit_sha256=self.digest,
            hardware=dict(config='B7',caps='0xd46f'),
            files=[dict(path='board/gfx2d_ti60.bit',sha256=self.digest,bytes=len(self.payload))]))
        self.assertEqual(subject.frozen_bit_identity(folder)[0],self.digest)
    def test_reject_modified_or_invalid_snapshot(self):
        _,original=self.snapshot()
        for name,value in [('config','B0'),('caps','0x346f'),('caps','broken'),
                           ('bit_sha256','0'*64),('bit_sha256','invalid'),('bit_bytes',1)]:
            with self.subTest(field=name,value=value):
                data=copy.deepcopy(original);data[name]=value;self.reject(data)
    def test_reject_missing_bit(self):
        folder,_=self.snapshot();(folder/'gfx2d_ti60.bit').unlink()
        with self.assertRaisesRegex(RuntimeError,'missing'):subject.frozen_bit_identity(folder)
    def test_reject_non_object(self):self.reject([])
    def test_reject_ambiguous_package(self):
        self.reject(dict(files=[dict(path='B7.bit',sha256=self.digest),dict(path='board/B7.bit',sha256=self.digest)]))
    def test_reject_manifest_path_escape(self):
        for path in ('../outside.bit','/tmp/outside.bit','C:/outside.bit'):
            with self.subTest(path=path):
                self.reject(dict(board_bit=path,files=[dict(path=path,sha256=self.digest)]))
    def test_reject_non_b7_package(self):
        self.reject(dict(board_bit='board/bit.bit',hardware=dict(config='B0',caps='0x40f'),
            files=[dict(path='board/bit.bit',sha256=self.digest)]))
    def tree(self,base=None,payload=None):
        tree=(base or self.base)/'tree';flow=tree/'fpga/gfx2d_ti60/outflow';flow.mkdir(parents=True)
        (flow/'gfx2d_ti60.bit').write_bytes(self.payload if payload is None else payload)
        (tree/'config.json').write_text(json.dumps(dict(caps='0xd46f')))
        (tree/'rtl').mkdir();(tree/'rtl/example.v').write_text('frozen RTL')
        return tree
    def test_frozen_allows_different_current_rtl(self):
        frozen,_=self.snapshot();tree=self.tree();root=self.base/'checkout'
        (root/'Main/rtl').mkdir(parents=True);(root/'Main/rtl/example.v').write_text('new RTL')
        with patch.object(subject,'ROOT',root),patch.object(subject,'collect',return_value={'verified':'reports'}):
            self.assertEqual(subject.b7_tree(tree,frozen)[0],tree)
            with self.assertRaises(subject.IncompleteCheck):subject.b7_tree(tree)
    def test_explicit_tree_never_falls_back_to_wrong_bit(self):
        frozen,_=self.snapshot();tree=self.tree(payload=b'other bit')
        with patch.object(subject,'collect') as collect:
            with self.assertRaises(subject.IncompleteCheck):subject.b7_tree(tree,frozen)
            collect.assert_not_called()
    def test_missing_reports_is_incomplete(self):
        frozen,_=self.snapshot();tree=self.tree()
        with patch.object(subject,'collect',side_effect=FileNotFoundError('timing')):
            with self.assertRaises(subject.IncompleteCheck):subject.b7_tree(tree,frozen)
    def test_bad_reports_are_not_silently_accepted(self):
        frozen,_=self.snapshot();tree=self.tree()
        with patch.object(subject,'collect',side_effect=RuntimeError('negative slack')):
            with self.assertRaisesRegex(RuntimeError,'negative slack'):subject.b7_tree(tree,frozen)
    def test_default_alias_uses_snapshot(self):
        import validate
        self.assertEqual(validate.CHECKS['r4-frozen-board']['command'][-1],'Main/bit/snapshots/B7')


def main():
    global TEST_BUILD
    current=session();TEST_BUILD=current.build('frozen-baseline');logs=current.logs('frozen-baseline')
    with (logs/'unit.log').open('w',encoding='utf-8') as stream:
        result=unittest.TextTestRunner(stream=stream,verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(FrozenIdentityTests))
    if not result.wasSuccessful():raise RuntimeError('frozen baseline unit cases failed; inspect unit.log')
    snapshot=ROOT/'Main/bit/snapshots/B7';digest,manifest=subject.frozen_bit_identity(snapshot)
    tree,timing=subject.b7_tree(None,snapshot)
    assert hashlib.sha256((tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit').read_bytes()).hexdigest()==digest
    for p in (manifest,snapshot/'gfx2d_ti60.bit',tree/'config.json'):current.artifact(p)
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,unit_cases=result.testsRun,
        frozen_bit_sha256=digest,tree=tree.relative_to(ROOT).as_posix(),timing=timing,
        scope='identity parser and report/tree selection; no hardware programming or current-RTL acceptance'))
    print('FROZEN BASELINE PASS formats/negatives/tree-selection; bit '+digest)


if __name__=='__main__':main()
