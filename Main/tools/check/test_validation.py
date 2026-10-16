#!/usr/bin/env python3
"""流程的行为测试：重复执行、缺项、旧证据与输入变更。"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import check_run
import validate


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        check_run.RUNS.mkdir(parents=True, exist_ok=True)
        self.directory=Path(tempfile.mkdtemp(prefix='test-',dir=check_run.RUNS))
        self.fixture=self.directory/'input.c'
        self.fixture.write_text('int value = 1;\n',encoding='utf-8')
        self.relative=self.fixture.relative_to(check_run.ROOT).as_posix()
        self.environment=patch.dict(os.environ,dict(os.environ))
        self.environment.start()
        for key in ('GFX_CHECK_RUN_DIR','GFX_CHECK_PROFILE','GFX_CHECK_SHARED'):
            os.environ.pop(key,None)
        self.inputs=patch.object(check_run,'source_files',return_value=[self.relative])
        self.inputs.start()
        self.cache=patch.object(check_run,'CACHE_DIR',self.directory/'cache')
        self.cache.start()
        self.current=check_run.Session(self.directory/'run','quick')
        self.singleton=patch.object(check_run,'_SESSION',self.current)
        self.singleton.start()

    def tearDown(self):
        self.singleton.stop()
        self.cache.stop()
        self.inputs.stop()
        self.environment.stop()
        shutil.rmtree(self.directory)

    def module(self,name):
        spec=importlib.util.spec_from_file_location('test_'+name,Path(__file__).with_name(name+'.py'))
        module=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_same_run_pass_executes_once(self):
        calls=[]
        self.current.perform('normal',lambda:calls.append(1))
        self.current.perform('normal',lambda:calls.append(2))
        self.assertEqual(calls,[1])

    def test_physical_checks_are_not_automatically_reused(self):
        calls=[]
        for _ in range(2):self.current.perform('board',lambda:calls.append(1),reusable=False)
        self.assertEqual(calls,[1,1])

    def test_feature_board_cli_reexecutes_physical_check_and_keeps_synthesis_budget(self):
        calls=[]
        def execute(args,log,**kwargs):
            calls.append((args[0],kwargs['timeout']))
            log.parent.mkdir(parents=True,exist_ok=True)
            log.write_text('PASS\n')
            return 'PASS\n'
        checks={
            'feature-synthesis':{'command':['fake-synthesis'],'purpose':'fixture',
                                 'dependencies':[],'inputs':[self.relative],'timeout':18000},
            'feature-board':{'command':['fake-board'],'purpose':'fixture',
                             'dependencies':['feature-synthesis'],'inputs':[self.relative],'timeout':3600}}
        argv=['validate.py','--checks','feature-board','--run','--run-dir',str(self.current.path)]
        with patch.dict(validate.CHECKS,checks),patch.object(validate,'Session',return_value=self.current), \
             patch.object(validate,'command',side_effect=execute),patch.object(sys,'argv',argv), \
             contextlib.redirect_stdout(io.StringIO()):
            validate.main()
            validate.main()
        self.assertEqual(calls,[('fake-synthesis',18000),('fake-board',3600),('fake-board',3600)])

    def test_compiler_reuse_ignores_gui_state_but_rejects_changed_mode_or_rtl(self):
        module=self.module('check_feature_synthesis')
        old=self.directory/'old';expected=self.directory/'expected'
        for tree in (old,expected):
            (tree/'rtl').mkdir(parents=True)
            (tree/'rtl/unit.v').write_text('module unit; endmodule\n')
            project=tree/'fpga/gfx2d_ti60';project.mkdir(parents=True)
            (project/'gfx2d_ti60.peri.xml').write_text('<peri/>')
            (project/'gfx2d_ti60.xml').write_text('<project last_run_state="pass"><mode value="speed"/></project>')
        xml=expected/'fpga/gfx2d_ti60/gfx2d_ti60.xml'
        xml.write_text('<project last_run_state=""><mode value="speed"/></project>')
        self.assertTrue(module.matching_inputs(old,expected)[0])
        xml.write_text('<project last_run_state="pass"><mode value="speed2"/></project>')
        self.assertFalse(module.matching_inputs(old,expected)[0])
        xml.write_text('<project last_run_state="pass"><mode value="speed"/></project>')
        source=expected/'rtl/unit.v';stamp=source.stat().st_mtime_ns
        source.write_text('module unit; wire changed; endmodule\n')
        os.utime(source,ns=(stamp,stamp))
        self.assertFalse(module.matching_inputs(old,expected)[0])

    def test_missing_output_is_incomplete_and_reexecuted(self):
        log=self.current.logs('runner')/'normal.log'
        def produce():
            log.write_text('PASS\n')
            return {'log':log.relative_to(check_run.ROOT).as_posix()}
        self.current.perform('normal',produce)
        log.unlink()
        with self.assertRaises(check_run.IncompleteCheck):self.current.require(['normal'])
        self.current.perform('normal',produce)
        self.assertTrue(log.exists())

    def test_changed_input_rejects_reuse(self):
        self.current.perform('normal',lambda:None)
        self.fixture.write_text('int value = 2;\n',encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError,'输入已改变'):
            self.current.perform('normal',lambda:self.fail('不应复用'))

    def test_old_pass_log_without_manifest_result_is_rejected(self):
        (self.current.logs('frontend')/'f5_bin.log').write_text('RESULT: PASS\n')
        with self.assertRaises(check_run.IncompleteCheck):self.current.require(['f5.bin'])

    def test_incomplete_cannot_be_collected(self):
        def missing():raise check_run.IncompleteCheck('toolchain missing')
        with self.assertRaises(check_run.IncompleteCheck):self.current.perform('target',missing)
        self.assertEqual(self.current.read()['checks']['target']['status'],'INCOMPLETE')
        with self.assertRaises(check_run.IncompleteCheck):self.current.require(['target'])

    def test_scope_cannot_change_inside_run(self):
        with self.assertRaisesRegex(RuntimeError,'运行参数已改变'):
            check_run.Session(self.current.path,'acceptance')

    def test_collectors_reject_quick_scope(self):
        for name in ['check_f4_instances','check_f5_bin']:
            with self.assertRaisesRegex(RuntimeError,'快速档'):self.module(name).collect()

    def test_instance_legacy_and_explicit_v2_are_deduplicated(self):
        module=self.module('check_f4_instances')
        calls=[]
        module.v2=lambda:calls.append('v2')
        module.models=lambda:None
        module.rtl=lambda:None
        def legacy():self.current.perform('f4.v2',module.v2)
        module.legacy=legacy
        with patch.object(sys,'argv',['check_f4_instances.py','--v2','--legacy']):module.main()
        self.assertEqual(calls,['v2'])

    def test_platform_and_explicit_hardware_are_deduplicated(self):
        module=self.module('check_f5_bin')
        calls=[]
        module.platform_hw=lambda:calls.append('hardware')
        module.platform=lambda:self.current.perform('f5.platform_hw',module.platform_hw)
        with patch.object(sys,'argv',['check_f5_bin.py','--platform','--platform-hw']):module.main()
        self.assertEqual(calls,['hardware'])

    def test_plan_deduplicates_dependencies(self):
        self.assertEqual(validate.expand(['bin-game','pack','bin-game']),['pack','bin-game'])
        self.assertEqual(validate.suggest(['Main/doc/开发环境.md']),['paths'])
        self.assertIn('display',validate.suggest(['Main/rtl/display/display_out.v']))
        with self.assertRaises(ValueError):validate.suggest(['unknown/module.c'])

    def test_input_scope_excludes_unrelated_source(self):
        other=self.directory/'other.v';other.write_text('module other; endmodule\n')
        names=[self.relative,other.relative_to(check_run.ROOT).as_posix()]
        with patch.object(check_run,'source_files',return_value=names):
            narrow=check_run.Session(self.directory/'narrow','quick',[self.relative])
            other.write_text('module changed; endmodule\n')
            narrow.verify()
            self.assertEqual(list(narrow.read()['inputs']),[self.relative])

    def test_identity_cache_reuses_unchanged_file_and_fresh_detects_preserved_timestamp(self):
        with patch.object(check_run,'input_identity',wraps=check_run.input_identity) as digest:
            check_run.Session(self.directory/'second','quick')
            self.assertEqual(digest.call_count,0)
            st=self.fixture.stat()
            self.fixture.write_text('int value = 2;\n')
            os.utime(self.fixture,ns=(st.st_atime_ns,st.st_mtime_ns))
            with self.assertRaisesRegex(RuntimeError,'输入已改变'):self.current.verify(fresh=True)
            self.assertEqual(digest.call_count,1)
            with self.assertRaisesRegex(RuntimeError,'输入已改变'):self.current.verify()

    def test_optimized_python_is_rejected_before_checks(self):
        source='import sys; sys.path.insert(0,sys.argv[1]); from check_run import Session; Session()'
        result=subprocess.run([sys.executable,'-O','-c',source,str(Path(__file__).parent)],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('禁止 python -O',result.stderr)

    @unittest.skipIf(os.name=='nt','shell 行为在 WSL 验证')
    def test_failed_shared_test_is_reexecuted(self):
        bindir=self.directory/'retry-bin';bindir.mkdir()
        counter=self.directory/'retry-counter'
        fake=bindir/'make'
        fake.write_text('#!/usr/bin/env bash\nprintf "call\\n" >> "$COUNTER"\nif [ "$(wc -l < "$COUNTER")" = 1 ]; then exit 1; fi\nfor arg; do case "$arg" in OBJDIR=*) dest="${arg#OBJDIR=}";; esac; done\nmkdir -p "$dest/test"\nprintf "fail 0\\ndone\\n" > "$dest/test/out64.txt"\ncp "$dest/test/out64.txt" "$dest/test/out32.txt"\n')
        fake.chmod(0o755)
        helper=Path(__file__).with_name('check_common.sh');dest=self.directory/'retry';dest.mkdir()
        env=dict(os.environ,PATH=str(bindir)+os.pathsep+os.environ['PATH'],COUNTER=str(counter),SW=str(dest),GFX_CHECK_SHARED=str(self.directory/'retry-shared'))
        result=subprocess.run(['bash','-c','source "$1"; check_libtest "$2/a" ""; test "$?" = 1 || exit 1; check_libtest "$2/b" ""','test',str(helper),str(dest)],env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(counter.read_text().splitlines(),['call','call'])

    def test_tracked_path_check_does_not_walk_ignored_directories(self):
        import localize_paths
        result=subprocess.CompletedProcess([],0,'Main/tools/check/regs_gen.py\0','')
        with patch.object(localize_paths.subprocess,'run',return_value=result),patch.object(localize_paths,'walk',side_effect=AssertionError('不应遍历工作区')):
            self.assertEqual(localize_paths.check(True),0)
        with patch.object(localize_paths.subprocess,'run',return_value=subprocess.CompletedProcess([],1,'','git failed')):
            with self.assertRaises(RuntimeError):localize_paths.check(True)

    def test_snapshot_keeps_incomplete_state_and_redacts_metadata_paths(self):
        from types import SimpleNamespace
        self.current.record('missing','INCOMPLETE',error='tool missing')
        (self.current.logs('runner')/'sample.log').write_text('original bytes\n')
        target=self.directory/'snapshot'
        validate.archive(SimpleNamespace(path=self.current.path),target,True)
        exported=json.loads((target/'run.json').read_text())
        self.assertEqual(exported['checks']['missing']['status'],'INCOMPLETE')
        self.assertFalse((target/'result.json').exists())
        self.assertFalse(json.loads((target/'archive.json').read_text())['current_inputs_revalidated'])
        from localize_paths import SUSPECT
        virtual_path=chr(90)+':'+chr(92)*2+'fixture'+chr(92)*2+'python.exe'
        self.assertIsNone(SUSPECT.search(validate.portable('Command ['+repr(virtual_path)+']')))

    def test_current_instance_collect_is_host_rtl_only(self):
        current=check_run.Session(self.directory/'formal','acceptance')
        with patch.object(check_run,'_SESSION',current):
            module=self.module('check_f4_instances')
            for name in ['models','rtl','mutations','protocols','lease']:current.perform('f4.'+name,lambda:None)
            module.collect()
            output=json.loads((module.LOGS/'f4_results.json').read_text())
            self.assertIn('host/RTL only',output['scope'])
            self.assertNotIn('board',output)

    def test_cross_clock_negative_slack_is_not_dropped(self):
        module=self.module('run_f5_synthesis')
        report='Setup (Max) Clock Relationship\ncore_clk other_clk 10.0 0.1 (ok)\nHold (Min) Clock Relationship\ncore_clk core_clk 0.0 0.02 (ok)\nNOTE: Values'
        rows=module.timing_slacks(report)
        self.assertEqual(len(rows),2)
        self.assertEqual(rows[0]['to_clock'],'other_clk')
        with self.assertRaisesRegex(RuntimeError,'negative slack'):module.timing_slacks(report.replace('10.0 0.1','10.0 -0.1'))

    def test_quick_instances_cover_equations_counts_layouts_and_spaces(self):
        cases=self.module('check_f4_instances').rtl_cases('quick')
        self.assertEqual(len(cases),25)
        self.assertEqual({n%6 for n in cases},set(range(6)))
        self.assertEqual({(n//12)%6 for n in cases},set(range(6)))
        self.assertEqual({(n//6)%2 for n in cases},{0,1})
        self.assertEqual({n//72 for n in cases},{0,1})

    def test_board_capture_requires_current_boundary_and_functional_completion(self):
        from check_board_smoke import validate_engine
        rows=[]
        for line in (check_run.ROOT/'Main/sw/lib/test/golden.txt').read_text().splitlines():
            name,crc=line.split()
            rows += [name+' '+crc+' '+crc+' ok','TEX-'+name+' '+crc+' '+crc+' ok']
        body='engineTest current\nENG id ok\n# --- 程序已加载，以下才是它的输出 ---\nCOUNT32 ok\nTEX-CAPS ok\nCAPS-BACKEND-PAIR ok\nTEX-LOAD ok\nPERF-BUSY ok\nSTRESS ok\n'+'\n'.join(rows)+'\nfail 0\ndone\n'
        self.assertEqual(validate_engine(body)['status'],'PASS')
        for changed in [body.replace('fail 0','fail 1'),body.replace('STRESS ok',''),
                        body.replace('CAPS-BACKEND-PAIR ok',''),
                        body.replace('done\n','nmax done\n'),body.replace('# --- 程序已加载，以下才是它的输出 ---',''),body+'# serial lost\n',
                        body.replace(rows[0]+'\n',''),body+rows[0]+'\n',body.replace(rows[0],rows[0].replace('9cd34cb7','00000000'))]:
            with self.assertRaises(RuntimeError):validate_engine(changed)

    @unittest.skipIf(os.name=='nt','shell 行为在 WSL 验证')
    def test_shell_status_contract(self):
        helper=Path(__file__).with_name('check_common.sh')
        for statement,expected,marker in [('FAIL=0',0,'VALIDATION PASS'),
            ('FAIL=1',1,'VALIDATION FAIL'),('FAIL=0; check_missing missing',3,'VALIDATION INCOMPLETE'),
            ('FAIL=0; check_na long_test',0,'NOT_APPLICABLE')]:
            result=subprocess.run(['bash','-c','source "$1"; '+statement+'; check_finish','test',str(helper)],capture_output=True,text=True)
            self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
            self.assertIn(marker,result.stdout)

    @unittest.skipIf(os.name=='nt','shell 行为在 WSL 验证')
    def test_shared_normal_test_runs_once_but_mutations_are_separate(self):
        bindir=self.directory/'bin';bindir.mkdir()
        counter=self.directory/'counter'
        fake=bindir/'make'
        fake.write_text('#!/usr/bin/env bash\nprintf "call\\n" >> "$COUNTER"\nfor arg; do case "$arg" in OBJDIR=*) dest="${arg#OBJDIR=}";; esac; done\nmkdir -p "$dest/test"\nprintf "fail 0\\ndone\\n" > "$dest/test/out64.txt"\ncp "$dest/test/out64.txt" "$dest/test/out32.txt"\n',encoding='utf-8')
        fake.chmod(0o755)
        helper=Path(__file__).with_name('check_common.sh')
        dest=self.directory/'calls';dest.mkdir()
        env=dict(os.environ,PATH=str(bindir)+os.pathsep+os.environ['PATH'],COUNTER=str(counter),SW=str(dest),GFX_CHECK_SHARED=str(self.directory/'shared'))
        result=subprocess.run(['bash','-c','source "$1"; check_libtest "$2/a" ""; check_libtest "$2/b" ""; check_libtest "$2/c" MUTANT','test',str(helper),str(dest)],env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(counter.read_text().splitlines(),['call','call'])
        self.assertIn('REUSE shared libtest',result.stdout)


if __name__=='__main__':
    with contextlib.redirect_stdout(io.StringIO()):
        suite=unittest.defaultTestLoader.loadTestsFromTestCase(WorkflowTests)
        result=unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
