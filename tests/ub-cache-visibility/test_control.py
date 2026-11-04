#!/usr/bin/env python3
"""Control-only regression: no SDK, SSH, memory probe, or resource-sensitive workload."""
import contextlib
import importlib.util
import io
import json
import tempfile
import sys
sys.dont_write_bytecode = True
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('visibility', Path(__file__).with_name('run.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ControlTest(unittest.TestCase):
    def scenario(self, stale, cleanup_failure=False):
        events = []
        class Process:
            def __init__(self, role):
                self.role = role
                self.stdin = io.StringIO()
            def wait(self, timeout):
                return 1 if cleanup_failure and self.role == 'A' else 0
            def terminate(self):
                events.append((self.role, 'terminate'))
        class Reader:
            def join(self, timeout):
                pass
        class Worker:
            def __init__(self, host, binary, role, args, log):
                self.role = role
                self.process = Process(role)
                self.reader = Reader()
                events.append((role, 'start'))
            def receive(self, args):
                return 'READY'
            def command(self, command, args):
                events.append((self.role, command))
                if command == 'prime':
                    return f'VALUE {runner.OLD}'
                if command == 'read':
                    return f'VALUE {runner.OLD if stale else runner.NEW}'
                if command == 'invalidate':
                    return f'VALUE {runner.NEW}'
                return 'CLOSED' if command in ('close', 'release') else 'OK'
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'result'
            argv = ['run.py', '--host-a', 'a', '--host-b', 'b', '--provider', 'b',
                    '--binary-a', '/worker', '--binary-b', '/worker', '--cpu-a', '0',
                    '--cpu-b', '0', '--output', str(out)]
            with patch('sys.argv', argv), patch.object(runner, 'Worker', Worker), \
                 contextlib.redirect_stdout(io.StringIO()):
                rc = runner.main()
            result = json.loads((out / 'result.json').read_text())
        return rc, result, events

    def test_order_and_success(self):
        rc, result, events = self.scenario(True)
        self.assertEqual(rc, 0)
        self.assertTrue(result['pass'])
        self.assertEqual(events, [('B', 'start'), ('A', 'start'), ('A', 'prime'),
                                 ('B', 'write'), ('A', 'read'), ('B', 'flush'),
                                 ('A', 'invalidate'), ('A', 'close'), ('B', 'release')])

    def test_early_new_is_not_pass(self):
        rc, result, _ = self.scenario(False)
        self.assertEqual(rc, 1)
        self.assertFalse(result['case1_old'])
        self.assertTrue(result['case2_new'])

    def test_failed_a_cleanup_retains_object(self):
        rc, result, events = self.scenario(True, cleanup_failure=True)
        self.assertEqual(rc, 1)
        self.assertNotIn(('B', 'release'), events)
        self.assertTrue(result['object_may_remain'])


if __name__ == '__main__':
    unittest.main()
