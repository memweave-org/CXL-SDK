#!/usr/bin/env python3
"""Thread-sweep control checks; no SDK, index operations, or measured workload."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import run_threads
from peer import check_thread_sweep

HERE = Path(__file__).resolve().parent


class ThreadSweepTests(unittest.TestCase):
    def test_default_entry_selects_requested_workers_and_original_workload(self):
        env = dict(os.environ)
        for key in ('YCSB_UB_CASE', 'YCSB_UB_SUITE', 'YCSB_UB_SINGLE', 'THREAD_COUNTS',
                    'YCSB_UB_CONFIG', 'YCSB_UB_FIXTURE', 'OPT', 'A_WRITABLE'):
            env.pop(key, None)
        completed = subprocess.run(['bash', str(HERE.parent / 'run.sh'), '--dry-run'],
                                   env=env, text=True, capture_output=True, check=True)
        plan = json.loads(completed.stdout)
        self.assertEqual(plan['rounds'], [{'threads_A': n, 'threads_B': n} for n in (32, 64, 128)])
        self.assertTrue(plan['opt'])
        self.assertFalse(plan['rpc'])
        self.assertFalse(plan['suite'])
        self.assertEqual(plan['A_workload'], '100% read')
        self.assertEqual(plan['update_semantics'], 'fresh-insert')

    def test_cpu_capacity_checked_for_largest_round(self):
        self.assertEqual(run_threads.check_cpus([32, 64, 128], 8, range(8, 136)), list(range(8, 136)))
        # A missing CPU number is not a missing CPU slot if enough others exist.
        sparse = set(range(50)) | set(range(96, 240))
        self.assertEqual(run_threads.check_cpus([32, 64, 128], 0, sparse),
                         list(range(50)) + list(range(96, 174)))
        for allowed in (range(8, 135), set(range(8, 136)) - {100}):
            with self.assertRaisesRegex(ValueError, '128-worker round.*available=127'):
                run_threads.check_cpus([32, 64, 128], 8, allowed)

    def test_cxl_entry_preserves_ab_workload_and_uses_32_workers_per_role(self):
        env={k:v for k,v in os.environ.items() if not k.startswith('YCSB_') and
             k not in ('THREAD_COUNTS','OPT','A_WRITABLE')}
        for entry in ([str(HERE.parent/'run.sh'),'--backend','cxl'],
                      [str(HERE.parent/'run_shm_ds.sh'),'-mode=cxl_two_process']):
            result=subprocess.run(['bash',*entry,'--dry-run'],env=env,text=True,capture_output=True,check=True)
            plan=json.loads(result.stdout)
            self.assertEqual(plan['backend'],'cxl')
            self.assertEqual(plan['cpu_numa_nodes'],{'A':0,'B':1})
            self.assertEqual(plan['rounds'],[{'threads_A':32,'threads_B':32}])
            self.assertEqual(plan['A_workload'],'100% read')
            self.assertEqual(plan['B_workload'],'90% read / 10% update')
            self.assertFalse(plan['rpc'])

    def test_cxl_pair_reuses_coordinator_and_separate_cpu_sets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); out=root/'results'; bundle=root/'bundle'
            (bundle/'lib').mkdir(parents=True); (bundle/'ycsbc').touch()
            calls=[]
            role_cpus={'A':list(range(24))+list(range(96,104)),
                       'B':list(range(24,48))+list(range(120,128))}
            def pair(commands):
                self.assertEqual(len(commands),2)
                envs={env['YCSB_UB_ROLE']:env for command,env,log in commands}
                self.assertEqual(set(envs),{'A','B'})
                for command,env,log in commands:
                    self.assertEqual(command,['taskset','-c',','.join(map(str,role_cpus[env['YCSB_UB_ROLE']])),
                                              'bash','run.sh'])
                    self.assertEqual(env['YCSB_BACKEND'],'cxl')
                    self.assertEqual(env['THREADS_A'],'32')
                    self.assertEqual(env['THREADS_B'],'32')
                self.assertEqual(envs['B']['CPU_B'],'24')
                self.assertEqual(envs['A']['CPU_A'],'0')
                self.assertEqual(envs['A']['YCSB_CXL_DIRECTORY'],envs['B']['YCSB_CXL_DIRECTORY'])
                result={'pass':True,'common_seconds':1.0,'total_mops':0.0002,
                        'A':{'reads':100,'writes':0,'bwtree_opt':True},
                        'B':{'reads':90,'writes':10,'bwtree_opt':True}}
                (Path(envs['A']['YCSB_UB_OUTPUT'])/'result.json').write_text(json.dumps(result))
                calls.append(envs)
            env={'SPEC':str(HERE/'workloadb_90read.spec'),'RECORDS':'100','OPS':'100'}
            with patch.dict(os.environ,env,clear=True),patch.object(run_threads,'run_commands',pair), \
                 patch.object(run_threads,'select_cxl_role_cpus',return_value=role_cpus), \
                 patch.object(run_threads.os,'sched_getaffinity',return_value=set(range(64))), \
                 patch.object(run_threads.tempfile,'mkdtemp',return_value=str(root/'backing')):
                (root/'backing').mkdir()
                self.assertEqual(run_threads.main(['--backend','cxl','--cxl-node','0','--thread-counts','32',
                                                   '--bundle',str(bundle),'--output',str(out)]),0)
            self.assertEqual(len(calls),1)
            self.assertFalse((root/'backing').exists())

    def test_handshake_rejects_different_rounds_or_counts(self):
        identity = {'thread_sweep': '{"counts":[32,64,128],"threads":32}', 'threads': 32}
        check_thread_sweep(identity, dict(identity))
        check_thread_sweep({'threads': 1}, {'threads': 2})  # Single runs may be asymmetric.
        for peer in ({**identity, 'threads': 64}, {**identity, 'thread_sweep': None},
                     {**identity, 'thread_sweep': 'another schedule'}):
            with self.assertRaises(RuntimeError):
                check_thread_sweep(identity, peer)
        with self.assertRaises(RuntimeError):
            check_thread_sweep({**identity, 'threads': 1}, {**identity, 'threads': 1})

    def test_cxl_rpc_entry_selects_local_partitions_and_noopt(self):
        env={k:v for k,v in os.environ.items() if not k.startswith('YCSB_') and
             k not in ('THREAD_COUNTS','OPT','A_WRITABLE')}
        env.update(YCSB_UB_CASE='bwtree+rpc', OPT='1')
        completed=subprocess.run(['bash',str(HERE.parent/'run.sh'),'--backend','cxl','--dry-run'],
                                 env=env,text=True,capture_output=True,check=True)
        plan=json.loads(completed.stdout)
        self.assertEqual(plan['case'],'bwtree+rpc')
        self.assertEqual(plan['rounds'],[{'threads_A':24,'threads_B':24}])
        self.assertEqual(plan['cpu_numa_nodes'],{'A':0,'B':1})
        self.assertEqual(plan['variant'],'cc_nocc_mq')
        self.assertEqual(plan['rpc_scheduler'],'original ThreadPool')
        self.assertTrue(plan['rpc'])
        self.assertTrue(plan['suite'])
        self.assertFalse(plan['opt'])
        self.assertEqual(plan['index_storage'],'local-dram')
        self.assertEqual(plan['rpc_transport'],'cxl-shared-queue')
        self.assertEqual(plan['update_semantics'],'replace-existing-key')

    def test_cxl_rpc_pair_keeps_case_and_checks_bidirectional_completion(self):
        result={'pass':True,'rpc':True,'common_seconds':1.0,'total_mops':0.0002}
        for role,sent,completed in [('A',43,56),('B',56,43)]:
            result[role]={'reads':100 if role=='A' else 90,'writes':0 if role=='A' else 10,
                          'bwtree_opt':False,'rpc_sent':sent,'rpc_expected_remote':sent,
                          'rpc_completed':completed,'rpc_local':100-sent,
                          'index_storage':'local-dram','rpc_transport':'original-mq-thread-pool'}
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); out=root/'results'; bundle=root/'bundle'
            (bundle/'lib').mkdir(parents=True); (bundle/'ycsbc').touch()
            (root/'backing').mkdir()
            calls=[]
            def pair(commands):
                for command,env,log in commands:
                    self.assertEqual(env['YCSB_UB_CASE'],'bwtree+rpc')
                    self.assertEqual(env['YCSB_UB_SUITE'],'1')
                    self.assertEqual(env['OPT'],'0')
                    self.assertEqual(env['YCSB_BACKEND'],'cxl')
                    if env['YCSB_UB_ROLE']=='A':
                        (Path(env['YCSB_UB_OUTPUT'])/'result.json').write_text(json.dumps(result))
                calls.append(commands)
            env={'SPEC':str(HERE/'workloadb_90read.spec'),'RECORDS':'100','OPS':'100',
                 'YCSB_UB_CASE':'bwtree+rpc','OPT':'1'}
            with patch.dict(os.environ,env,clear=True),patch.object(run_threads,'run_commands',pair), \
                 patch.object(run_threads,'select_cxl_role_cpus',return_value={'A':[0,1,2,3],'B':[4,5,6,7]}), \
                 patch.object(run_threads.os,'sched_getaffinity',return_value=set(range(8))), \
                 patch.object(run_threads.tempfile,'mkdtemp',return_value=str(root/'backing')):
                self.assertEqual(run_threads.main(['--backend','cxl','--cxl-node','0','--thread-counts','2',
                                                  '--bundle',str(bundle),'--output',str(out)]),0)
            self.assertEqual(len(calls),1)
            self.assertFalse((root/'backing').exists())
        for change in ({'rpc_completed':0},{'rpc_sent':44},{'rpc_local':58},
                       {'index_storage':'shared'},{'rpc_transport':'none'}):
            with self.subTest(change=change),self.assertRaisesRegex(RuntimeError,'RPC'):
                run_threads.validate_result({**result,'A':{**result['A'],**change}},0,True,100)
        with self.assertRaisesRegex(RuntimeError,'RPC'):
            run_threads.validate_result({**result,'rpc':False},0,True,100)

    def test_rounds_reuse_one_build_and_stop_on_failure(self):
        for fail_at in (None, 64):
            with self.subTest(fail_at=fail_at), tempfile.TemporaryDirectory() as temporary:
                out = Path(temporary) / 'results'
                calls = []

                def run_command(command, env, log):
                    if command[1] == 'build.sh':
                        calls.append('build')
                        bundle = Path(command[2])
                        (bundle / 'lib').mkdir(parents=True)
                        (bundle / 'ycsbc').touch()
                        return
                    n = int(env['THREADS_A'])
                    calls.append(n)
                    assert env['THREADS_B'] == str(n)
                    assert env['YCSB_UB_SUITE'] == '0' and env['YCSB_UB_CASE'] == 'bwtree'
                    assert json.loads(env['YCSB_UB_THREAD_SWEEP'])['threads'] == n
                    if n == fail_at:
                        raise RuntimeError('injected round failure')
                    result = {'pass': True, 'common_seconds': 1.0, 'total_mops': 0.0002,
                              'A': {'reads': 100, 'writes': 0, 'bwtree_opt': True, 'rpc_completed': 0},
                              'B': {'reads': 90, 'writes': 10, 'bwtree_opt': True, 'rpc_completed': 0}}
                    (Path(env['YCSB_UB_OUTPUT']) / 'result.json').write_text(json.dumps(result))

                env = {'YCSB_UB_ROLE': 'A', 'SPEC': str(HERE / 'workloadb_90read.spec'),
                       'RECORDS': '100', 'OPS': '100', 'OPT': '1'}
                with patch.dict(os.environ, env, clear=True), \
                     patch.object(run_threads, 'run_command', run_command), \
                     patch.object(run_threads.os, 'sched_getaffinity', return_value=set(range(128))):
                    code = run_threads.main(['--output', str(out)])
                report = json.loads((out / 'summary.json').read_text())
                if fail_at is None:
                    self.assertEqual(code, 0)
                    self.assertEqual(calls, ['build', 32, 64, 128])
                    self.assertEqual(len(report['rounds']), 3)
                else:
                    self.assertEqual(code, 1)
                    self.assertEqual(calls, ['build', 32, 64])
                    self.assertEqual(len(report['rounds']), 1)
                    self.assertTrue((out / 'threads-64' / 'invalid.json').is_file())
                    self.assertFalse((out / 'threads-128').exists())


if __name__ == '__main__':
    unittest.main()
