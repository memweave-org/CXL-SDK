#!/usr/bin/env python3
"""Protocol and arithmetic tests only; no SDK, cache probe, or performance workload."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode=True
from peer import summary
HERE=Path(__file__).resolve().parent
HARNESS=r'''
import io, json, queue, sys
sys.dont_write_bytecode=True
import peer
role=sys.argv[sys.argv.index('--role')+1]
peer.socket.gethostname=lambda:'fake-'+role
class Process:
    stdin=io.StringIO()
    def wait(self,timeout): return 0
    def terminate(self): pass
class Reader:
    def join(self,timeout): pass
    def is_alive(self): return False
class Worker:
    def __init__(self,args,name,provider,log):
        assert provider=='fake-B'
        self.process=Process();self.reader=Reader();self.q=queue.Queue()
        self.q.put('READY')
    def receive(self): return self.q.get(timeout=5)
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

    def test_two_peers(self):
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
                        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True))
                for child in children:
                    output,_=child.communicate(timeout=15)
                    self.assertEqual(child.returncode,0,output)
                a,b=[json.loads((root/role/'result.json').read_text()) for role in ('A','B')]
                self.assertEqual(a,b)
                self.assertTrue(a['pass'])
                self.assertEqual(a['framework'], 'tests/YCSB-C/ycsbc')
                self.assertEqual(a['total_mops'], 0.02)
            finally:
                for child in children:
                    if child.poll() is None: child.kill()
                    child.wait()

if __name__=='__main__': unittest.main()
