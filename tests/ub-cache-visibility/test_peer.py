#!/usr/bin/env python3
"""TCP control integration only: replace SDK workers."""
import json
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
HARNESS = r'''
import io, sys
sys.dont_write_bytecode = True
import peer
role = sys.argv[sys.argv.index('--role') + 1]
peer.socket.gethostname = lambda: 'fake-host-' + role
class Process:
    stdin = io.StringIO()
    pid = 99999999
    def wait(self, timeout): return 0
    def poll(self): return 0
    def terminate(self): pass
class Reader:
    def join(self, timeout): pass
    def is_alive(self): return False
class Worker:
    def __init__(self, *a):
        assert a[3].provider == 'fake-host-B', a[3].provider
        self.process, self.reader = Process(), Reader()
    def receive(self, *a): return 'READY'
    def command(self, cmd, *a):
        if cmd in ('prime', 'read'): return 'VALUE ' + str(peer.OLD)
        if cmd == 'invalidate': return 'VALUE ' + str(peer.NEW)
        return 'CLOSED' if cmd in ('close', 'release') else 'OK'
peer.Worker = Worker
raise SystemExit(peer.main())
'''


class PeerTest(unittest.TestCase):
    def test_two_local_controllers(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            processes = []
            try:
                for role in ('B', 'A'):
                    out = Path(directory) / role
                    out.mkdir()
                    processes.append(subprocess.Popen(
                        [sys.executable, '-c', HARNESS, '--role', role, '--peer', '127.0.0.1',
                         '--port', str(port), '--provider', '', '--binary', '/unused',
                         '--output', str(out)], cwd=HERE,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
                for process in processes:
                    output, _ = process.communicate(timeout=15)
                    self.assertEqual(process.returncode, 0, output)
                a, b = [json.loads((Path(directory) / role / 'result.json').read_text())
                        for role in ('A', 'B')]
                self.assertEqual(a, b)
                self.assertTrue(a['pass'])
                self.assertTrue(a['case1_old'])
                self.assertTrue(a['case2_new'])
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.wait()


if __name__ == '__main__':
    unittest.main()
