"""跑器的行为反例：污染、异常、假检出、空身份及移位后的真实执行。"""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('campaign',Path(__file__).with_name('run_mutation.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class CampaignTests(unittest.TestCase):
    def test_two_targets_and_exception_restore(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'a').write_bytes(b'a');(root/'b').write_bytes(b'b')
            original={'a':b'a','b':b'b'}
            for name in original:
                row={'target':name,'rules':[{'find':name,'replace':'wrong'}]}
                with self.assertRaisesRegex(RuntimeError,'consumer failed'):
                    with m.mutation(root,row,original):
                        self.assertEqual((root/name).read_bytes(),b'wrong')
                        self.assertEqual((root/('b' if name=='a' else 'a')).read_bytes(),original['b' if name=='a' else 'a'])
                        raise RuntimeError('consumer failed')
                self.assertEqual({n:(root/n).read_bytes() for n in original},original)

    def test_rule_unique_and_not_empty(self):
        for raw,rules in [(b'aa',[{'find':'a','replace':'b'}]),(b'a',[{'find':'a','replace':'a'}])]:
            with self.assertRaises(ValueError):m.mutated_bytes(raw,rules)

    def test_foreign_target_deletion_restores_everything(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'a').write_bytes(b'a');(root/'b').write_bytes(b'b')
            with self.assertRaisesRegex(RuntimeError,'未登记'):
                with m.mutation(root,{'target':'a','rules':[{'find':'a','replace':'wrong'}]}, {'a':b'a','b':b'b'}):
                    (root/'b').unlink()
            self.assertEqual((root/'a').read_bytes(),b'a')
            self.assertEqual((root/'b').read_bytes(),b'b')

    def test_infrastructure_and_timeouts_are_not_kills(self):
        for text in ['fatal error: header absent CHECK_MISMATCH','syntax error CHECK_MISMATCH','Unable to open CHECK_MISMATCH',
                     'unit.v:12: error: Unable to bind CHECK_MISMATCH']:
            self.assertEqual(m.classify({'timeout':False,'returncode':1,'text':text},'CHECK_MISMATCH'),'INVALID')
        self.assertEqual(m.classify({'timeout':True,'returncode':-9,'text':'CHECK_MISMATCH'},'CHECK_MISMATCH'),'KILLED-WEAK')
        self.assertEqual(m.classify({'timeout':False,'returncode':0,'text':'CHECK_MISMATCH'},'CHECK_MISMATCH'),'SURVIVED')
        self.assertEqual(m.classify({'timeout':False,'returncode':1,'text':'OTHER_FAIL'},'CHECK_MISMATCH'),'KILLED-WEAK')
        self.assertEqual(m.classify({'timeout':False,'returncode':1,'text':'CHECK_MISMATCH'},'CHECK_MISMATCH'),'KILLED')

    def test_partial_and_weak_block_acceptance(self):
        for states in [[],['INVALID'],['KILLED','SURVIVED'],['KILLED-WEAK'],['SURVIVED']]:
            self.assertNotEqual(m.aggregate(states),'KILLED')

    def test_source_manifest_relocates_and_detects_missing(self):
        import check_run
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'Main').mkdir();(root/'Main/input.c').write_text('value')
            (root/'Main/LICENSE.MD').write_text('license')
            p=root/'context.json';p.write_text(json.dumps({'files':['Main/input.c','Main/LICENSE.MD'],'revision':'frozen'}))
            with patch.object(check_run,'ROOT',root),patch.dict(os.environ,{'GFX_CHECK_SOURCE_MANIFEST':str(p)}):
                self.assertEqual(check_run.source_files(),['Main/LICENSE.MD','Main/input.c'])
                (root/'Main/input.c').unlink()
                with self.assertRaises(RuntimeError):check_run.source_files()
                p.write_text(json.dumps({'files':[]}))
                with self.assertRaises(RuntimeError):check_run.source_files()

    def test_execution_preserves_full_output_and_timeout(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'Main').mkdir()
            result=m.execute(root,"python3 -c 'print(\"x\"*210000)'",root/'logs1','quick',10)
            self.assertEqual(result['returncode'],0)
            self.assertGreater((root/'logs1/stdout.log').stat().st_size,200000)
            result=m.execute(root,"python3 -c 'import time;print(\"started\",flush=True);time.sleep(10)'",root/'logs2','quick',0.1)
            self.assertTrue(result['timeout'])
            self.assertIn('started',(root/'logs2/stdout.log').read_text())

    def test_deleted_tracked_text_is_not_a_current_input(self):
        import check_run,localize_paths
        with tempfile.TemporaryDirectory() as folder:
            with patch.object(check_run,'ROOT',Path(folder)),patch.object(check_run,'source_files',return_value=[]), \
                 patch.object(localize_paths,'candidates',return_value=[('Main/removed.py',None)]):
                self.assertEqual(check_run.selected_sources(['@tracked-text']),[])


if __name__=='__main__':unittest.main()
