#!/usr/bin/env python3
"""Untimed real-worker/TCP regression using a local shared-file SDK fixture.

Synthetic hostnames allow two controllers on one host. This does not validate
physical UB, NC mapping attributes, or cross-machine atomicity.
"""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
HARNESS = r'''
import os, sys
sys.dont_write_bytecode = True
import peer
role = sys.argv[sys.argv.index('--role') + 1]
peer.socket.gethostname = lambda: 'fixture-host-' + role
fault = os.environ.get('NC_TEST_FAULT', '')
class Worker(peer.Worker):
    def command(self, command, args):
        response = super().command(command, args)
        if role != os.environ.get('NC_TEST_FAULT_ROLE', 'A'):
            return response
        if command.startswith('cas '):
            _, success, observed = response.split()
            if fault == 'cas_success_flag' and success == '1':
                return 'CAS 0 ' + observed
            if fault == 'cas_failure_value' and success == '0':
                return 'CAS 0 ' + command.split()[1]
            if fault == 'cas_failure_write' and success == '0':
                # An illegal write on failed comparison, retaining its correct reply.
                super().command('cas ' + observed + ' ' + command.split()[2], args)
        if command.startswith('fetch_add '):
            old = int(response.split()[1])
            new = (old + int(command.split()[1])) & ((1 << 64) - 1)
            if fault == 'fetch_add_new_value':
                return 'FETCH_ADD ' + str(new)
            if fault == 'fetch_add_no_write':
                super().command(f'cas {new} {old}', args)
        if fault == 'close_failure' and command == 'close':
            raise RuntimeError('injected A cleanup failure')
        return response
peer.Worker = Worker
raise SystemExit(peer.main())
'''


class NcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='nc-fixture-build-')
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / 'worker')
        mock = HERE.parent / 'ub-api' / 'mock'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-I' + str(mock),
                        str(HERE / 'worker.cc'), str(mock / 'sdk.cc'),
                        '-o', cls.binary], check=True, capture_output=True, text=True)

    def scenario(self, fault='', role='A'):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        with tempfile.TemporaryDirectory(prefix='nc-fixture-run-') as directory:
            root = Path(directory)
            env = dict(os.environ, UB_MICRO_NC='1', NC_MAP_MODE='noreplace',
                       NC_SIZE_MB='4', NC_TIMEOUT='5',
                       UBSM_TEST_DIRECTORY=str(root / 'objects'),
                       NC_TEST_FAULT=fault, NC_TEST_FAULT_ROLE=role)
            for key in ('UBSM_TEST_HANG', 'UBSM_TEST_FAIL', 'UBSM_TEST_WRONG_ADDRESS'):
                env.pop(key, None)
            processes, output = {}, {}
            try:
                for host in ('B', 'A'):
                    out = root / host
                    out.mkdir()
                    with (out / 'stdout.log').open('w') as stdout:
                        processes[host] = subprocess.Popen(
                            [sys.executable, '-c', HARNESS, '--role', host,
                             '--peer', '127.0.0.1', '--port', str(port), '--provider', '',
                             '--binary', self.binary, '--output', str(out)],
                            env=env, cwd=HERE, stdout=stdout, stderr=subprocess.STDOUT,
                            start_new_session=True)
                for host, process in processes.items():
                    process.wait(timeout=20)
                    output[host] = (root / host / 'stdout.log').read_text()
                reports = {host: json.loads((root / host / 'result.json').read_text())
                           for host in processes}
                logs = {host: (root / host / 'worker.log').read_text() for host in processes}
                codes = {host: process.returncode for host, process in processes.items()}
                objects = list((root / 'objects').iterdir())
                return reports, logs, codes, objects, output
            finally:
                for process in processes.values():
                    # Only our controller's process group, including its worker.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()

    def test_bidirectional_read_write_cas_fetch_add(self):
        reports, logs, codes, objects, output = self.scenario()
        self.assertEqual(codes, {'A': 0, 'B': 0}, output)
        self.assertEqual(reports['A'], reports['B'])
        report = reports['A']
        self.assertTrue(report['pass'])
        self.assertEqual(report['final'], 2)
        self.assertEqual(len(report['checks']), 32)
        self.assertTrue(all(item['pass'] for item in report['checks'].values()))
        for host in ('A', 'B'):
            for op in ('cas_success', 'cas_failure', 'fetch_add', 'fetch_add_wrap'):
                self.assertEqual(report['checks'][f'{host}_{op}']['role'], host)
            self.assertNotIn('BEGIN ownership', logs[host])
            self.assertNotIn('BEGIN command flush', logs[host])
            self.assertNotIn('BEGIN command invalidate', logs[host])
        self.assertEqual(objects, [])

    def test_incorrect_returns_and_memory_updates_fail_on_either_host(self):
        for host in ('A', 'B'):
            for fault in ('cas_success_flag', 'cas_failure_value', 'cas_failure_write',
                          'fetch_add_new_value', 'fetch_add_no_write'):
                with self.subTest(host=host, fault=fault):
                    reports, logs, codes, objects, output = self.scenario(fault, host)
                    self.assertEqual(codes, {'A': 1, 'B': 1}, output)
                    self.assertFalse(reports['A']['pass'])
                    self.assertFalse(reports['B']['pass'])
                    self.assertIn('expected', reports['A']['error'])
                    self.assertTrue(reports['A']['object_may_remain'])
                    self.assertTrue(objects)
                    self.assertNotIn('BEGIN command release', logs['B'])

    def test_a_cleanup_failure_prevents_release(self):
        reports, logs, codes, objects, output = self.scenario('close_failure')
        self.assertEqual(codes, {'A': 1, 'B': 1}, output)
        self.assertFalse(reports['A']['pass'])
        self.assertIn('cleanup failure', reports['A']['error'])
        self.assertNotIn('BEGIN command release', logs['B'])
        self.assertTrue(objects)


if __name__ == '__main__':
    unittest.main()
