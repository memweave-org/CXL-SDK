#!/usr/bin/env python3
"""Exercise original MQ across two processes using a prebuilt build.sh rpc bundle.

For UB, --fixture requires a file-backed SDK and makes no hardware claim.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import signal
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--backend', choices=('cxl', 'ub'), required=True)
    parser.add_argument('--fixture', action='store_true')
    parser.add_argument('--cxl-node', default='4')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.backend == 'ub' and not args.fixture:
        parser.error('Local UB test requires --fixture and a file-backed SDK')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 10:
        parser.error('Need 10 allowed CPUs for distinct clients and services')
    reports = []
    for a, b in ((1, 1), (2, 2), (3, 2)):
        directory = out / f'A{a}-B{b}'
        directory.mkdir()
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        # A separate object directory makes accidental fixture reuse impossible.
        objects = directory / 'objects'
        objects.mkdir()
        cxl_objects = Path(tempfile.mkdtemp(prefix='ycsb-mq-contract-', dir='/dev/shm')) if args.backend == 'cxl' else objects
        env = dict(os.environ, YCSB_BACKEND=args.backend, YCSB_UB_CASE='bwtree+rpc',
                   YCSB_UB_BUNDLE=str(args.bundle.resolve()), B_IP='127.0.0.1',
                   PORT=str(port), THREADS_A=str(a), THREADS_B=str(b),
                   CPU_B=str(cpus[0]), CPU_A=str(cpus[2*b]), RECORDS='1024', OPS='2000',
                   SIZE_MB='1024', NC_SIZE_MB='256', YCSB_UB_FIXTURE=str(int(args.fixture)),
                   UBSM_TEST_DIRECTORY=str(objects), YCSB_CXL_DIRECTORY=str(cxl_objects),
                   YCSB_CXL_NODE=args.cxl_node)
        children, logs = [], []
        try:
            for role in ('B', 'A'):
                log = (directory / f'{role}.log').open('w')
                logs.append(log)
                children.append(subprocess.Popen(['bash', str(HERE/'run.sh')],
                    env=dict(env, YCSB_UB_ROLE=role, YCSB_UB_OUTPUT=str(directory/role)),
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True))
            for child in children:
                if child.wait(timeout=120):
                    raise RuntimeError(f'MQ process failed; see {directory}')
            report = json.loads((directory/'A/result.json').read_text())
            assert report['pass'] and report['rpc']
            for role in ('A', 'B'):
                peer = 'B' if role == 'A' else 'A'
                result = report[role]
                assert result['rpc_transport'] == 'original-mq-thread-pool'
                assert result['errors'] == 0 and result['reads'] + result['writes'] == 2000
                assert result['rpc_sent'] == report[peer]['rpc_completed']
                text = (directory/role/'worker.log').read_text()
                assert 'transport=original-mq-thread-pool' in text
            reports.append({'threads_A': a, 'threads_B': b, 'result': report})
            print(f'PASS {args.backend}: A={a}, B={b}, original MQ, 4000 operations', flush=True)
        finally:
            for child in children:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
            for log in logs:
                log.close()
            if args.backend == 'cxl':
                shutil.rmtree(cxl_objects)
    (out/'verification.json').write_text(json.dumps({
        'pass': True, 'backend': args.backend, 'fixture': args.fixture,
        'implementation': 'cc_nocc_mq / original ThreadPool / original MPMCQueue',
        'rounds': reports}, indent=2)+'\n')


if __name__ == '__main__':
    main()
