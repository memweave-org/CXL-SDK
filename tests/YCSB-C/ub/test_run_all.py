#!/usr/bin/env python3
"""Run the real bash run_all.sh -> run.sh -> peer.py flow with SDK fixture bundles.
One CPU, 35 records, 128 operations per host. No hardware/performance claim.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundles',type=Path,required=True)
    p.add_argument('--output',type=Path)
    args=p.parse_args()
    root=(args.output or Path(tempfile.mkdtemp(prefix='ub-run-all-fixture-'))).resolve()
    root.mkdir(parents=True,exist_ok=True)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    env=dict(os.environ,UBSM_TEST_DIRECTORY=str(root/'objects'),B_IP='127.0.0.1',
             PORT=str(port),CPU_A=str(min(os.sched_getaffinity(0))),CPU_B=str(min(os.sched_getaffinity(0))),
             THREADS_A='1',THREADS_B='1',RECORDS='35',OPS='128',SIZE_MB='128',NC_SIZE_MB='128',A_WRITABLE='0')
    children=[]; logs=[]
    try:
        for role in ('B','A'):
            log=(root/(role+'.log')).open('w'); logs.append(log)
            children.append(subprocess.Popen(['bash',str(HERE.parent/'run_all.sh'),'--fixture',
                '--bundles',str(args.bundles.resolve()),'--output',str(root/role)],
                env=dict(env,YCSB_UB_ROLE=role),stdout=log,stderr=subprocess.STDOUT))
        for child in children:
            assert child.wait(timeout=300)==0, f'run_all failed, see {root}'
        for role in ('A','B'):
            result=json.loads((root/role/'summary.json').read_text())
            expected=['bwtree','bwtree+opt','bwtree+rpc','clevelhash','clevelhash+opt',
                      'clevelhash+rpc','sherman+nc-direct','sherman+nc-memcpy']
            assert result['pass'] and [case['case'] for case in result['cases']]==expected,result
            for case in result['cases']:
                assert case['bwtree_opt']==case['case'].endswith('+opt'),case
                assert case['update_semantics']=='replace-existing-key',case
                for host in ('A','B'):
                    assert case[host]['errors']==0 and case[host]['reads']+case[host]['writes']==128
                assert case['A']['reads']==128 and case['A']['writes']==0,case
                assert case['B']['writes']>0,case
                text=(root/role/case['case']/'worker.log').read_text()
                nc=case['case'].startswith('sherman+nc-')
                assert f'CACHE={"NONE" if nc else "READ" if role=="A" else "RW"} NC=RW' in text
                if nc:
                    assert case['B']['rpc_completed']==0,case
                    assert case['cache_policy']=='NC-only; A reads, B reads/updates',case
                if case['rpc']: assert case['B']['rpc_completed']==128
                else: assert case['B']['rpc_completed']==0
        assert not list((root/'objects').iterdir()),'UB fixture objects leaked'
        print('PASS: all eight cases, native two-process shell flow, one CPU; logs:',root)
    finally:
        for child in children:
            if child.poll() is None: child.send_signal(signal.SIGINT)
            try: child.wait(timeout=10)
            except subprocess.TimeoutExpired: child.kill(); child.wait()
        for log in logs: log.close()

if __name__=='__main__': main()
