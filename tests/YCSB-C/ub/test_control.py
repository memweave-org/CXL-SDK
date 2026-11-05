#!/usr/bin/env python3
"""Protocol and arithmetic tests only; no SDK, cache probe, or performance workload."""
import json
import io
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace
sys.dont_write_bytecode=True
from peer import summary, Worker
HERE=Path(__file__).resolve().parent
HARNESS=r'''
import io, json, os, queue, sys
sys.dont_write_bytecode=True
import peer
role=sys.argv[sys.argv.index('--role')+1]
peer.socket.gethostname=lambda:'fake-'+role
class Process:
    stdin=io.StringIO()
    def poll(self): return 0
    def wait(self,timeout): return 0
    def terminate(self): pass
class Reader:
    def join(self,timeout): pass
    def is_alive(self): return False
class Worker:
    def __init__(self,args,name,provider,log):
        assert provider=='fake-B'
        if os.environ.get('FAIL_ROLE') == role:
            raise RuntimeError('injected UB map failure on '+role)
        self.process=Process();self.reader=Reader();self.protocol_reader=Reader();self.q=queue.Queue()
        self.q.put('READY')
    def receive(self): return self.q.get(timeout=5)
    def check_alive(self): pass
    def send(self,command):
        assert command=='run'
        result={'role':role,'reads':100 if role=='A' else 90,
                'writes':0 if role=='A' else 10,'errors':0,'seconds':0.01,
                'bwtree_opt': True, 'framework_total_operations': 200 if role=='B' else 0}
        self.q.put('RESULT '+json.dumps(result))
    def command(self,command,expected):
        raise AssertionError('No substitute lookup loop is permitted')
    def finish(self,command):
        assert command==('close' if role=='A' else 'release')
peer.Worker=Worker
raise SystemExit(peer.main())
'''

class ControlTests(unittest.TestCase):
    def test_fault_reports_address_and_region(self):
        worker=Worker.__new__(Worker)
        worker.role='A';worker.binary='unused'
        worker.data_size=worker.nc_size=128<<20
        with patch('peer.shutil.which',return_value=None):
            for address,region in [('0x300000000040','CACHE data'),
                                   ('0x400000000040','NC metadata'),('0x0','outside UB')]:
                with self.subTest(address=address):
                    with self.assertRaisesRegex(RuntimeError,region) as raised:
                        worker.raise_fault(f'FAULT signal=0xb code=0x2 address={address} pc=0x401234')
                    self.assertIn(address,str(raised.exception))
                    self.assertIn('pc=0x401234',str(raised.exception))

    def test_running_worker_failure_stops_peer(self):
        for role, drop_report in (('A',False),('B',False),('A',True)):
            with self.subTest(role=role,drop_report=drop_report), tempfile.TemporaryDirectory() as directory:
                root=Path(directory)
                binary=root/'worker'
                binary.write_text('#!'+sys.executable+'\n'+r'''
import os, signal, sys, time
from pathlib import Path
role='B' if sys.argv[sys.argv.index('-machineno')+1]=='0' else 'A'
Path(os.environ['PID_DIR'],role+'.pid').write_text(str(os.getpid()))
fd=int(os.environ['YCSB_UB_CONTROL_FD'])
os.write(fd,b'READY\n')
assert sys.stdin.readline().strip()=='run'
if role==os.environ['CRASH_ROLE']:
    os._exit(17)
# Force the controller to exercise SIGKILL escalation, not just graceful stdin EOF.
signal.signal(signal.SIGTERM,signal.SIG_IGN)
while True: time.sleep(1)
''')
                binary.chmod(0o700)
                spec=root/'spec';spec.write_text('unused')
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
                children=[]
                started=time.monotonic()
                try:
                    for host in ('B','A'):
                        output=root/host;output.mkdir()
                        harness=r'''
import os, sys, peer
role=sys.argv[sys.argv.index('--role')+1]
peer.socket.gethostname=lambda:'fake-'+role
original_send=peer.PeerChannel.send
def send(channel,message):
    if os.environ['DROP_REPORT']=='1' and role==os.environ['CRASH_ROLE'] and isinstance(message,dict) and message.get('type')=='ERROR':
        channel.connection.shutdown(peer.socket.SHUT_RDWR)
        raise OSError('injected disconnect before error report')
    return original_send(channel,message)
peer.PeerChannel.send=send
sys.exit(peer.main())
'''
                        children.append(subprocess.Popen([sys.executable,'-c',harness,
                            '--role',host,'--peer','127.0.0.1','--port',str(port),
                            '--binary',str(binary),'--spec',str(spec),'--ini',str(spec),
                            '--timeout','60','--output',str(output)],cwd=HERE,
                            env=dict(os.environ,CRASH_ROLE=role,PID_DIR=str(root),DROP_REPORT=str(int(drop_report))),
                            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True))
                    for child in children:
                        output,_=child.communicate(timeout=10)
                        self.assertEqual(child.returncode,1,output)
                    self.assertLess(time.monotonic()-started,10)
                    for host in ('A','B'):
                        report=json.loads((root/host/'result.json').read_text())
                        self.assertFalse(report['pass'])
                        expected='peer disconnected' if drop_report and host!=role else 'worker exited with code 17'
                        self.assertIn(expected,report['error'])
                        pid=int((root/(host+'.pid')).read_text())
                        with self.assertRaises(ProcessLookupError): os.kill(pid,0)
                finally:
                    for child in children:
                        if child.poll() is None: child.kill()
                        child.wait()
                    for path in root.glob('*.pid'):
                        try: os.kill(int(path.read_text()),9)
                        except ProcessLookupError: pass

    def test_control_pipe_ignores_sdk_output(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'noisy-worker'
            binary.write_text('#!'+sys.executable+'\n'+r'''
import os, sys
fd=int(os.environ['YCSB_UB_CONTROL_FD'])
# Fake protocol lines and an unterminated SDK line must not drive the handshake.
os.write(1,b'UBYCSB CLOSED\nUBYCSB READY\nSDK partial output: ')
os.write(2,b'concurrent SDK output without newline')
os.write(fd,b'RE')
os.write(fd,b'ADY\n')
assert sys.stdin.readline().strip()=='run'
os.write(1,b'UBYCSB RESULT broken-json\n')
os.write(fd,b'RESULT {"ok":true}\n')
assert sys.stdin.readline().strip()=='close'
os.write(fd,b'CLOSED\n')
''')
            binary.chmod(0o700)
            args=SimpleNamespace(role='A',binary=str(binary),threads_a=1,threads_b=1,
                size_mb=128,nc_size_mb=128,records=2,ops=1,cpu=0,timeout=3,
                spec='unused',ini='unused')
            log=io.StringIO()
            worker=Worker(args,'unused','unused',log)
            try:
                self.assertEqual(worker.receive(),'READY')
                worker.send('run')
                self.assertEqual(worker.receive(),'RESULT {"ok":true}')
                worker.finish('close')
                worker.reader.join(timeout=1)
                self.assertIn('UBYCSB RESULT broken-json',log.getvalue())
                with self.assertRaisesRegex(RuntimeError,'worker exited'):
                    worker.receive()
            finally:
                if worker.process.poll() is None: worker.process.kill()
                worker.process.wait(timeout=3)
                worker.reader.join(timeout=1);worker.protocol_reader.join(timeout=1)

    def test_common_window(self):
        a={'role':'A','reads':100,'writes':0,'errors':0,'bwtree_opt':True}
        b={'role':'B','reads':90,'writes':10,'errors':0,'bwtree_opt':True,'framework_total_operations':200,'seconds':2}
        result=summary(a,b,7,100)
        self.assertEqual(result["controller_seconds"],7)
        self.assertEqual(result["common_seconds"],2)
        self.assertEqual(result['total_ops_per_second'],100)
        self.assertEqual(result['B_observed_read_fraction'],0.90)
        with self.assertRaises(RuntimeError): summary(a,{**b,'errors':1},2,100)
        with self.assertRaises(RuntimeError): summary({**a,'writes':1},b,2,100)

    def test_suite_requires_both_workloads_and_correct_rpc_count(self):
        a={'role':'A','reads':90,'writes':10,'errors':0,'bwtree_opt':False}
        b={**a,'role':'B','framework_total_operations':200,'seconds':2,'rpc_completed':10}
        self.assertEqual(summary(a,b,3,100,suite=True)['update_semantics'],'replace-existing-key')
        with self.assertRaisesRegex(RuntimeError,'RPC completion'):
            summary(a,b,3,100,suite=True,rpc=1)
        self.assertTrue(summary(a,{**b,'rpc_completed':100},3,100,suite=True,rpc=1)['pass'])

    def test_two_peers(self):
        a,b=self.run_two_peers()
        self.assertEqual(a,b)
        self.assertTrue(a['pass'])
        self.assertEqual(a['framework'], 'tests/YCSB-C/ycsbc')
        self.assertEqual(a['total_mops'], 0.02)

    def test_peer_receives_original_failure(self):
        for role in ('A','B'):
            with self.subTest(role=role):
                reports=self.run_two_peers(role)
                for report in reports:
                    self.assertFalse(report['pass'])
                    self.assertIn('injected UB map failure on '+role,report['error'])
                peer=reports[1 if role=='A' else 0]
                self.assertEqual(peer['peer_error']['role'],role)

    def run_two_peers(self, fail_role=''):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);binary=root/'worker';binary.write_text('fake')
            spec=root/'spec';spec.write_text('fake')
            children=[]
            try:
                for role in ('B','A'):
                    output=root/role;output.mkdir()
                    children.append(subprocess.Popen([sys.executable,'-c',HARNESS,
                        '--role',role,'--peer','127.0.0.1','--port',str(port),
                        '--binary',str(binary),'--spec',str(spec),'--ini',str(spec),'--records','100',
                        '--ops','100','--output',str(output)],cwd=HERE,
                        env=dict(os.environ,FAIL_ROLE=fail_role),
                        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True))
                for child in children:
                    output,_=child.communicate(timeout=15)
                    self.assertEqual(child.returncode,1 if fail_role else 0,output)
                a,b=[json.loads((root/role/'result.json').read_text()) for role in ('A','B')]
                return a,b
            finally:
                for child in children:
                    if child.poll() is None: child.kill()
                    child.wait()

if __name__=='__main__': unittest.main()
