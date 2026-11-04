#!/usr/bin/env python3
"""Tiny correctness test: 32 initial keys, 128 operations per process, no performance claims.
Uses a file-backed SDK fixture, not real UB hardware. Timings are discarded.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
sys.dont_write_bytecode=True
from peer import Worker
HERE=Path(__file__).resolve().parent
ROOT=HERE.parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix='ub-bwtree-ro-') as directory:
        tmp=Path(directory)
        source=(HERE/'mock/sdk.cc').read_text()
        (tmp/'sdk.cc').write_text(source)
        sdk=tmp/'libubsm_sdk.so'
        subprocess.run(['c++','-std=c++17','-shared','-fPIC','-I'+str(HERE/'mock'),
                        str(tmp/'sdk.cc'),'-o',str(sdk)],check=True)
        env=dict(os.environ,UBSM_INCLUDE_DIR=str(HERE/'mock'),UBSM_LIBRARY=str(sdk),MAPPING_BITS='10')
        binary=tmp/'worker'
        subprocess.run(['bash',str(HERE/'build.sh'),str(binary)],env=env,check=True)
        os.environ['LD_LIBRARY_PATH']=str(tmp)+':'+os.environ.get('LD_LIBRARY_PATH','')
        os.environ['UBSM_TEST_DIRECTORY']=str(tmp/'objects')
        spec=tmp/'workload.spec'
        spec.write_text('readproportion=0\nupdateproportion=1\nrequestdistribution=uniform\n')
        cpu=min(os.sched_getaffinity(0))
        workers=[]; logs=[]
        try:
            for role in ('B','A'):
                log=(tmp/(role+'.log')).open('w');logs.append(log)
                args=SimpleNamespace(role=role,binary=str(binary),size_mb=128,threads=1,
                                     records=32,ops=128,spec=str(spec),cpu=cpu,timeout=30)
                worker=Worker(args,'readonly-test','fixture-owner',log);workers.append(worker)
                assert worker.receive()=='READY'
            b,a=workers
            a.send('run');b.send('run')
            results=[json.loads(w.receive().removeprefix('RESULT ')) for w in (a,b)]
            assert results[0]['reads']==128 and results[0]['writes']==0 and results[0]['errors']==0
            assert results[1]['reads']==0 and results[1]['writes']==128 and results[1]['errors']==0
            a.command('verify '+' '.join(map(str,range(1,161))),'VERIFIED')
            a.finish('close');b.finish('release')
            assert not list((tmp/'objects').iterdir())
            assert 'prot=1' in (tmp/'A.log').read_text()
            print('PASS: A PROT_READ, B RW, reads/inserts and cross-process visibility; fixture only')
        except Exception:
            for role in ('A','B'):
                path=tmp/(role+'.log')
                if path.exists(): print(path.read_text(),file=sys.stderr)
            raise
        finally:
            for w in workers:
                if w.process.poll() is None:
                    w.process.terminate()
                w.process.wait(timeout=5)
                w.reader.join(timeout=1)
            for log in logs:log.close()


if __name__=='__main__':main()
