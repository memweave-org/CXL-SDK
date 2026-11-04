#!/usr/bin/env python3
"""Tiny original-framework correctness test with a file-backed SDK fixture.
No hardware/performance conclusions; all timings are discarded.
Pass --binary built with BWTREE_MAPPING_TABLE_BITS=10 and the fixture SDK.
"""
import argparse
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
from peer import Worker
HERE=Path(__file__).resolve().parent

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--opt',type=int,choices=(0,1),required=True)
    args=parser.parse_args()
    cpus=os.sched_getaffinity(0)
    cpu=next(c for c in sorted(cpus) if c+1 in cpus)
    with tempfile.TemporaryDirectory(prefix='native-ycsb-ub-') as directory:
        root=Path(directory)
        os.environ['UBSM_TEST_DIRECTORY']=str(root/'objects')
        workers=[];logs=[]
        try:
            for role in ('B','A'):
                log=(root/(role+'.log')).open('w');logs.append(log)
                settings=SimpleNamespace(role=role,binary=args.binary,threads_a=1,threads_b=2,
                    size_mb=128,nc_size_mb=128,records=35,ops=128,cpu=cpu,timeout=30,
                    spec=str(HERE/'workloadb_90read.spec'),ini=str(HERE/'config.ini'))
                worker=Worker(settings,'native-framework-test','fixture',log)
                workers.append(worker)
                assert worker.receive()=='READY'
            b,a=workers
            a.send('run');b.send('run')
            ar=json.loads(a.receive().removeprefix('RESULT '))
            br=json.loads(b.receive().removeprefix('RESULT '))
            assert ar['reads']==128 and ar['writes']==0 and ar['errors']==0, ar
            assert br['reads']+br['writes']==128 and br['errors']==0, br
            assert br['framework_total_operations']==256, br
            assert ar['bwtree_opt']==br['bwtree_opt']==bool(args.opt)
            a.finish('close');b.finish('release')
            assert not list((root/'objects').iterdir())
            atext=(root/'A.log').read_text();btext=(root/'B.log').read_text()
            for log in (atext,btext):
                calls=[line for line in log.splitlines() if line.startswith('UB_MAP_CALL ')]
                assert len(calls)==2, calls
                assert 'index=1 name=native-framework-test-nc' in calls[0], calls
                assert 'index=2 name=native-framework-test' in calls[1], calls
            assert 'YCSB_STAGE A local threads ready' in atext
            assert 'address=0x300000000000 prot=1' in atext
            assert 'address=0x400000000000 prot=3' in atext
            assert 'address=0x300000000000 prot=3' in btext
            assert '# Loaded records:\t35' in btext
            assert 'throughput\t' in btext and 'total operations\t256' in btext
            print('PASS: native ycsbc -> LoadManager/MasterTransactionManager/Client/BwTreeDB; OPT='+str(args.opt))
        except BaseException:
            for log in logs:log.flush()
            for role in ('A','B'):
                path=root/(role+'.log')
                if path.exists(): print(path.read_text())
            raise
        finally:
            for worker in workers:
                if worker.process.poll() is None: worker.process.terminate()
                worker.process.wait(timeout=5);worker.reader.join(timeout=1)
                worker.protocol_reader.join(timeout=1)
            for log in logs:log.close()
if __name__=='__main__':main()
