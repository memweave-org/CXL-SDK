#!/usr/bin/env python3
"""Check shell routing without SDK mappings, index operations, or measurements."""
import json
import csv
import io
from contextlib import redirect_stdout
from unittest.mock import patch
import run_all
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
EXPECTED={
    'bwtree': ('bwtree',0,0,'cache',0),
    'bwtree+opt': ('bwtree',1,0,'cache',0),
    'bwtree+rpc': ('bwtree',0,1,'cache',0),
    'clevelhash': ('clevelhash',0,0,'cache',0),
    'clevelhash+opt': ('clevelhash',1,0,'cache',0),
    'clevelhash+rpc': ('clevelhash',0,1,'cache',0),
    'sherman+nc-direct': ('sherman',0,0,'nc-direct',0),
    'sherman+nc-memcpy': ('sherman',0,0,'nc-memcpy',0),
}


class RoutingTests(unittest.TestCase):
    def test_cxl_plan_uses_same_cases_workload_and_memory_as_ub(self):
        env={k:v for k,v in os.environ.items() if k not in ('THREAD_COUNTS','YCSB_BACKEND')}
        plans={}
        for backend in ('cxl','ub'):
            result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--backend',backend,
                                   '--thread-counts','32','--dry-run'],
                                  env=env,capture_output=True,text=True,check=True)
            plans[backend]=json.loads(result.stdout)
        for key in ('cases','rounds','common','memory_by_case','workload','update_semantics'):
            self.assertEqual(plans['cxl'][key],plans['ub'][key])
        self.assertEqual(plans['cxl']['process_model'],'A/B independent processes')

    def test_cxl_runs_all_cases_as_two_processes_and_reuses_bundles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); out=root/'results'; bundles=root/'bundles'
            for mode in ('noopt','opt','rpc'):
                (bundles/mode/'lib').mkdir(parents=True)
                (bundles/mode/'ycsbc').touch()
            calls=[]
            role_cpus={'A':list(range(24))+list(range(96,120)),
                       'B':list(range(24,48))+list(range(120,144))}
            def pair(commands):
                self.assertEqual(len(commands),2)
                for command,env,_ in commands:
                    self.assertEqual(command,['taskset','-c',','.join(map(str,role_cpus[env['YCSB_UB_ROLE']])),
                                              'bash','run.sh'])
                envs={env['YCSB_UB_ROLE']:env for _,env,_ in commands}
                a,b=envs['A'],envs['B']; case=a['YCSB_UB_CASE']
                self.assertEqual(a['YCSB_BACKEND'],'cxl')
                self.assertEqual(a['CPU_A'],'0'); self.assertEqual(b['CPU_B'],'24')
                self.assertEqual(a['YCSB_CXL_DIRECTORY'],b['YCSB_CXL_DIRECTORY'])
                self.assertEqual(a['THREADS_A'],'24'); self.assertEqual(b['THREADS_B'],'24')
                self.assertEqual(a['SIZE_MB'],'49152'); self.assertEqual(a['NC_SIZE_MB'],'16384')
                self.assertEqual(Path(a['YCSB_UB_BUNDLE']).name,
                    'rpc' if case.endswith('+rpc') else 'opt' if case.endswith('+opt') else 'noopt')
                result={'pass':True,'case':case,'bwtree_opt':case.endswith('+opt'),
                        'cache_policy':'test','common_seconds':1,'total_mops':0.0002,
                        'A':{'reads':100,'writes':0,'trace_hash':'A'},
                        'B':{'reads':90,'writes':10,'trace_hash':'B','rpc_completed':0}}
                (Path(a['YCSB_UB_OUTPUT'])/'result.json').write_text(json.dumps(result))
                calls.append(case)
            # Each completed round removes its empty backing directory.
            def make_backing(**kwargs):
                directory=root/'backing'; directory.mkdir(); return str(directory)
            env={'SPEC':str(HERE/'workloadb_90read.spec'),'A_IP':'unused','B_IP':'unused',
                 'RECORDS':'100','OPS':'100','SIZE_MB':'49152','NC_SIZE_MB':'16384',
                 'SEED':'1','A_WRITABLE':'0'}
            with patch.dict(os.environ,env,clear=True),patch.object(run_all,'run_commands',pair), \
                 patch.object(run_all,'select_cxl_role_cpus',return_value=role_cpus), \
                 patch.object(run_all.os,'sched_getaffinity',return_value=set(range(192))), \
                 patch.object(run_all.tempfile,'mkdtemp',side_effect=make_backing),redirect_stdout(io.StringIO()):
                self.assertEqual(run_all.main(['--backend','cxl','--cxl-node','0','--bundles',str(bundles),
                                               '--output',str(out)]),0)
            self.assertEqual(calls,list(EXPECTED))
            self.assertTrue(json.loads((out/'summary.json').read_text())['pass'])
            self.assertFalse((root/'backing').exists())

    def test_memory_defaults_and_overrides(self):
        base={k:v for k,v in os.environ.items() if k not in
              ('SIZE_MB','NC_SIZE_MB','SHERMAN_NC_SIZE_MB','YCSB_UB_CONFIG','A_WRITABLE')}
        for override,cache,nc,sherman_nc in (
            ({},49152,16384,65536),
            ({'NC_SIZE_MB':'128'},49152,128,128),
            ({'SIZE_MB':'256','SHERMAN_NC_SIZE_MB':'512'},256,16384,512),
            ({'NC_SIZE_MB':'128','SHERMAN_NC_SIZE_MB':'512'},49152,128,512),
        ):
            with self.subTest(override=override):
                result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--dry-run'],
                    env=dict(base,**override),capture_output=True,text=True,check=True)
                plan=json.loads(result.stdout)
                for case in EXPECTED:
                    self.assertEqual(plan['memory_by_case'][case],{
                        'cache_mb':0 if case.startswith('sherman+') or case.endswith('+rpc') else cache,
                        'nc_mb':sherman_nc if case.startswith('sherman+') else nc})
                # run_all.sh exports config before run.sh sources it again.
                result=subprocess.run(['bash','-c',
                    'set -a; source "$1"; source "$1"; printf "%s %s %s" "$SIZE_MB" "$NC_SIZE_MB" "$SHERMAN_NC_SIZE_MB"',
                    'config-test',str(HERE/'config.sh')],env=dict(base,**override),
                    capture_output=True,text=True,check=True)
                self.assertEqual(result.stdout,f'{cache} {nc} {sherman_nc}')

    def test_batch_plan(self):
        result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--dry-run'],
                              env={k:v for k,v in os.environ.items() if k not in ('THREAD_COUNTS','YCSB_UB_CONFIG','A_WRITABLE','PROVIDER_B','PROVIDER_CACHE','PROVIDER_NC')},
                              capture_output=True,text=True,check=True)
        plan=json.loads(result.stdout)
        self.assertEqual(plan['cases'],list(EXPECTED))
        self.assertEqual(plan['providers'],{'cache':'B hostname','nc':'B hostname'})
        self.assertEqual(plan['thread_counts'],[32,64,128])
        self.assertEqual(plan['rounds'],[{'case':c,'threads_A':n,'threads_B':n} for c in EXPECTED for n in (32,64,128)])
        self.assertEqual(plan['A_workload'],'100% read')
        self.assertEqual(plan['B_workload'],'90% read / 10% update')
        self.assertEqual(plan['sherman']['writers'],'B only, no RPC')

    def test_thread_count_overrides_and_validation(self):
        for counts in ('32 64','1','128 32'):
            result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--dry-run'],
                env=dict(os.environ,THREAD_COUNTS=counts),capture_output=True,text=True,check=True)
            self.assertEqual(json.loads(result.stdout)['thread_counts'],[int(n) for n in counts.split()])
        result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--dry-run','--thread-counts','64'],
            env=dict(os.environ,THREAD_COUNTS='32 128'),capture_output=True,text=True,check=True)
        self.assertEqual(json.loads(result.stdout)['thread_counts'],[64])
        for counts in ('0','193','32 32','invalid'):
            result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--dry-run'],
                env=dict(os.environ,THREAD_COUNTS=counts),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)

    def test_matrix_reuses_builds_and_stops_on_failure_or_trace_mismatch(self):
        expected=[(case,n) for case in EXPECTED for n in (32,64,128)]
        for fault in (None,'command','trace'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temporary:
                out=Path(temporary)/'results'; calls=[]
                def run_command(command,env,log):
                    if command[1]=='build.sh':
                        calls.append(('build',env['OPT'],env['YCSB_RPC_BUILD']))
                        bundle=Path(command[2]);(bundle/'lib').mkdir(parents=True);(bundle/'ycsbc').touch()
                        return
                    case=env['YCSB_UB_CASE'];n=int(env['THREADS_A']);calls.append((case,n))
                    self.assertEqual(env['THREADS_B'],str(n))
                    self.assertEqual(env['CPU_A'],'0')
                    self.assertEqual(env['YCSB_UB_SUITE'],'1')
                    self.assertEqual(env['OPS'],'100')
                    self.assertEqual(env['SIZE_MB'],'128')
                    self.assertEqual(env['NC_SIZE_MB'],'512' if case.startswith('sherman+') else '128')
                    self.assertEqual(Path(env['YCSB_UB_BUNDLE']).name,'rpc' if case.endswith('+rpc') else 'opt' if case.endswith('+opt') else 'noopt')
                    schedule=json.loads(env['YCSB_UB_THREAD_SWEEP'])
                    self.assertEqual(schedule,{'runner':'run_all','cases':list(EXPECTED),
                        'counts':[32,64,128],'case':case,'threads':n})
                    if fault=='command' and (case,n)==('bwtree',64):raise RuntimeError('injected command failure')
                    # Different thread counts intentionally have different hashes.
                    trace=f'trace-{n}'
                    if fault=='trace' and (case,n)==('bwtree+opt',64):trace='wrong-trace'
                    result={'pass':True,'case':case,'bwtree_opt':case.endswith('+opt'),
                            'cache_policy':'test','common_seconds':1,'total_mops':0.0002,
                            'A':{'reads':100,'writes':0,'trace_hash':trace+'-A'},
                            'B':{'reads':90,'writes':10,'trace_hash':trace+'-B',
                                 'rpc_completed':100 if case.endswith('+rpc') else 0}}
                    (Path(env['YCSB_UB_OUTPUT'])/'result.json').write_text(json.dumps(result))
                env={'YCSB_UB_ROLE':'A','SPEC':str(HERE/'workloadb_90read.spec'),
                     'A_IP':'unused','B_IP':'unused','RECORDS':'100','OPS':'100','SIZE_MB':'128',
                     'NC_SIZE_MB':'128','SHERMAN_NC_SIZE_MB':'512','SEED':'1','A_WRITABLE':'0','THREADS_A':'7','THREADS_B':'9'}
                with patch.dict(os.environ,env,clear=True), patch.object(run_all,'run_command',run_command), \
                     patch.object(run_all.os,'sched_getaffinity',return_value=set(range(256))), \
                     patch.object(run_all.os,'getloadavg',side_effect=AssertionError('load detection forbidden')), \
                     patch.object(Path,'glob',side_effect=AssertionError('process scanning forbidden')),redirect_stdout(io.StringIO()):
                    code=run_all.main(['--output',str(out)])
                report=json.loads((out/'summary.json').read_text())
                self.assertEqual(calls[:3],[('build','0','0'),('build','1','0'),('build','0','1')])
                if fault is None:
                    self.assertEqual(code,0);self.assertTrue(report['pass']);self.assertEqual(calls[3:],expected)
                    self.assertEqual([(r['case'],r['threads_A']) for r in report['cases']],expected)
                    with (out/'summary.csv').open() as stream:rows=list(csv.DictReader(stream))
                    self.assertEqual(len(rows),24)
                    self.assertEqual([(r['case'],int(r['threads_A']),int(r['threads_B'])) for r in rows],
                                     [(c,n,n) for c,n in expected])
                    for case,n in expected:
                        result=json.loads((out/case/f'threads-{n}'/'result.json').read_text())
                        self.assertEqual((result['threads_A'],result['threads_B']),(n,n))
                        self.assertEqual(result['memory']['nc_mb'],
                                         512 if case.startswith('sherman+') else 128)
                else:
                    stop=('bwtree',64) if fault=='command' else ('bwtree+opt',64)
                    completed=expected.index(stop)
                    self.assertEqual(code,1);self.assertEqual(calls[3:],expected[:completed+1])
                    self.assertEqual(len(report['cases']),completed)
                    self.assertTrue((out/stop[0]/f'threads-{stop[1]}'/'invalid.json').is_file())
                    self.assertFalse((out/stop[0]/'threads-128').exists())
                self.assertFalse((out/'exclusivity.jsonl').exists())

    def test_run_command_never_queries_external_load(self):
        with tempfile.TemporaryDirectory() as temporary:
            with (Path(temporary)/'command.log').open('w') as log, \
                 patch.object(run_all.os,'getloadavg',side_effect=AssertionError('load detection forbidden')), \
                 patch.object(Path,'glob',side_effect=AssertionError('process scanning forbidden')):
                run_all.run_command([sys.executable,'-c','pass'],dict(os.environ),log)
                with self.assertRaisesRegex(RuntimeError,'Command exited 7'):
                    run_all.run_command([sys.executable,'-c','raise SystemExit(7)'],dict(os.environ),log)

    def test_pair_failure_stops_other_child_and_preserves_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); pidfile=root/'child.pid'
            waiting='import os,time,pathlib; pathlib.Path('+repr(str(pidfile))+').write_text(str(os.getpid())); print("waiting for peer",flush=True); time.sleep(30)'
            failing='import time,pathlib\np=pathlib.Path('+repr(str(pidfile))+')\nwhile not p.exists(): time.sleep(.01)\nprint("peer failed",flush=True)\nraise SystemExit(17)'
            with (root/'A.log').open('w') as a,(root/'B.log').open('w') as b,redirect_stdout(io.StringIO()) as terminal:
                with self.assertRaisesRegex(RuntimeError,'Command exited 17'):
                    run_all.run_commands([([sys.executable,'-c',waiting],dict(os.environ),a),
                                          ([sys.executable,'-c',failing],dict(os.environ),b)])
            self.assertIn('peer failed',terminal.getvalue())
            self.assertIn('peer failed',(root/'B.log').read_text())
            with self.assertRaises(ProcessLookupError): os.kill(int(pidfile.read_text()),0)

    def test_a_writable_is_rejected_before_build_or_run(self):
        for script,arguments in [('run.sh',[]),('run_all.sh',['--dry-run'])]:
            result=subprocess.run(['bash',str(HERE/script),*arguments],
                env=dict(os.environ,A_WRITABLE='1'),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('A is always read-only',result.stderr)

    def test_cases_and_legacy_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); tools=root/'tools'; tools.mkdir()
            for name in ('run.sh','config.sh'):
                shutil.copy(HERE/name,root/name)
            (root/'build.sh').write_text('''#!/bin/bash
set -eu
mkdir -p "$1/lib"
touch "$1/ycsbc"
printf '%s %s %s' "$OPT" "$YCSB_INDEXES" "$YCSB_RPC_BUILD" > "$1/build-routing"
''')
            (tools/'ldd').write_text('#!/bin/sh\nexit 0\n')
            (tools/'python3').write_text('#!'+sys.executable+'\n'+
                'import json,os,sys\nfrom pathlib import Path\n'+
                'Path(os.environ["YCSB_UB_OUTPUT"],"peer-args.json").write_text(json.dumps(sys.argv[1:]))\n')
            for script in tools.iterdir(): script.chmod(0o700)
            for case,expected in list(EXPECTED.items())+[('legacy',('bwtree',1,0,'cache',0))]:
                with self.subTest(case=case):
                    out=root/case; out.mkdir()
                    env=dict(os.environ,PATH=str(tools)+':'+os.environ['PATH'],
                        YCSB_UB_ROLE='A',YCSB_UB_CASE='bwtree' if case=='legacy' else case,
                        YCSB_UB_SUITE='0' if case=='legacy' else '1',
                        YCSB_UB_OUTPUT=str(out),OPT='1',A_WRITABLE='0',
                        PROVIDER_CACHE='provider-82',PROVIDER_NC='provider-81')
                    for key in ('YCSB_UB_BUNDLE','YCSB_UB_FIXTURE','YCSB_UB_CONFIG',
                                'SIZE_MB','NC_SIZE_MB','SHERMAN_NC_SIZE_MB'):
                        env.pop(key,None)
                    subprocess.run(['bash',str(root/'run.sh')],env=env,check=True,capture_output=True)
                    db,opt,rpc,access,writable=expected
                    index='clevel-hash' if db=='clevelhash' else db
                    self.assertEqual((out/'build-routing').read_text(),f'{opt} {index} {rpc}')
                    args=json.loads((out/'peer-args.json').read_text())
                    def value(flag): return args[args.index(flag)+1]
                    self.assertEqual(value('--expected-opt'),str(opt))
                    self.assertEqual(value('--size-mb'),'49152')
                    self.assertEqual(value('--nc-size-mb'),'65536' if case.startswith('sherman+nc-') else '16384')
                    self.assertEqual(value('--cache-provider'),'provider-82')
                    self.assertEqual(value('--nc-provider'),'provider-81')
                    self.assertEqual('--suite' in args,case!='legacy')
                    if case!='legacy':
                        self.assertEqual(value('--db'),db)
                        self.assertEqual(value('--rpc'),str(rpc))
                        self.assertEqual(value('--sherman-access'),access)
                        self.assertEqual(value('--a-writable'),str(writable))


if __name__=='__main__': unittest.main()
