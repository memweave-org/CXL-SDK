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
