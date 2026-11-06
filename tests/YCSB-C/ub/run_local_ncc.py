#!/usr/bin/env python3
"""Run original A/B BwTree/ClevelHash with independent CACHE replicas (correctness only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import signal
import subprocess
import time
from types import SimpleNamespace
from local_ncc_guard import ResourceGuard
from peer import Worker

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--opt', type=int, choices=(0, 1), default=1)
    parser.add_argument('--db', choices=('bwtree', 'clevelhash'), default='bwtree')
    parser.add_argument('--suite', action=argparse.BooleanOptionalAction, default=True,
                        help='A only reads directly; B reads/updates existing keys')
    parser.add_argument('--seed', type=int, default=20261104)
    parser.add_argument('--initial-cache', choices=('cold', 'stale'), default='stale', help='Initial state on BOTH A and B')
    parser.add_argument('--startup-invalidate', action=argparse.BooleanOptionalAction, default=None,
                        help='Override normal UBSM startup policy; default tests the production default')
    parser.add_argument('--read-ahead-lines', type=int, default=0, help='Simulate next-line prefetch on each access (0..64)')
    parser.add_argument('--check-reads', action=argparse.BooleanOptionalAction, default=True,
                        help='Fail on stale A demand bytes without repairing them')
    parser.add_argument('--trace', action='store_true', help='Retain every cacheline event, including cold fills')
    parser.add_argument('--drop', choices=('writeback', 'invalidate'), help='Negative control: suppress these transfers')
    parser.add_argument('--records', type=int, default=1000)
    parser.add_argument('--value-size', type=int, default=8, help='CoW payload bytes; use >64 to cover multiple cachelines')
    parser.add_argument('--ops', type=int, default=2000, help='Operations per host')
    parser.add_argument('--size-mb', type=int, default=128)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    binary = args.build.resolve()/('ycsbc_nocc' if args.opt else 'ycsbc_nocc_no_opt')
    contract = args.build.resolve()/'local_ncc_contract'
    if not binary.is_file() or not contract.is_file():
        parser.error('Build the LOCAL_NCC ycsbc and local_ncc_contract targets first')
    cache = (args.build/'CMakeCache.txt').read_text()
    if 'YCSB_UB_LOCAL_NCC:BOOL=ON' not in cache:
        parser.error('Refusing a binary without YCSB_UB_LOCAL_NCC=ON')
    if args.records < 2 or args.ops < 1 or not 0 <= args.read_ahead_lines <= 64 or not 8 <= args.value_size <= 1048576:
        parser.error('records >= 2, ops >= 1, value-size in 8..1048576 and read-ahead-lines in 0..64 are required')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cpu = min(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {cpu})
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.environ['UBSM_TEST_DIRECTORY'] = str(output/'objects')
    os.environ['LOCAL_NCC_INITIAL'] = args.initial_cache
    os.environ['LOCAL_NCC_READ_AHEAD'] = str(args.read_ahead_lines)
    os.environ['LOCAL_NCC_CHECK_READS'] = str(int(args.check_reads))
    for key in ('LOCAL_NCC_STARTUP_INVALIDATE', 'LOCAL_NCC_TRACE_DIR', 'LOCAL_NCC_DROP',
                'YCSB_UB_SUITE', 'YCSB_UB_A_WRITABLE', 'YCSB_UB_RPC'):
        os.environ.pop(key, None)
    os.environ.pop('SHM_UB_STARTUP_INVALIDATE', None)
    if args.startup_invalidate is not None:
        os.environ['SHM_UB_STARTUP_INVALIDATE'] = str(int(args.startup_invalidate))
    # BwTree follows the original startup-only reader invalidation protocol.
    os.environ['SHM_UB_COW_ACQUIRE'] = '0'
    if args.trace:
        os.environ['LOCAL_NCC_TRACE_DIR'] = str(output)
    if args.drop:
        os.environ['LOCAL_NCC_DROP'] = args.drop
    result = dict(pass_=False, performance_valid=False, model='single-writer-cacheline-replicas-v2',
                  db=args.db, suite=args.suite, seed=args.seed,
                  initial_cache=args.initial_cache, startup_invalidate=args.startup_invalidate is not False,
                  startup_policy='production-default' if args.startup_invalidate is None else 'explicit-override',
                  read_ahead_lines=args.read_ahead_lines, check_reads=args.check_reads,
                  cow_acquire=False, empty_lines_fill_on_read=True,
                  fault_injection=args.drop, cpu=cpu, threads_a=1, threads_b=1,
                  records=args.records, ops_per_host=args.ops, value_size=args.value_size, binary=str(binary),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    result['pass'] = result.pop('pass_')
    guard = ResourceGuard(output)
    workers, logs = [], []
    contract_process = None
    try:
        guard.start()
        with (output/'contract.log').open('w') as log:
            contract_process = subprocess.Popen([str(contract)], stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic()+args.timeout
            while contract_process.poll() is None:
                guard.check()
                if time.monotonic() > deadline:
                    raise TimeoutError('local_ncc_contract timed out')
                time.sleep(.1)
            if contract_process.returncode:
                raise RuntimeError('Replica contract failed; see contract.log')
        result['replica_contract'] = 'pass'
        for role in ('B', 'A'):
            guard.check()
            log = (output/(role+'.log')).open('w'); logs.append(log)
            settings = SimpleNamespace(role=role, binary=str(binary), threads_a=1, threads_b=1,
                db=args.db, suite=args.suite, rpc=0, a_writable=0, seed=args.seed, sherman_access='cache',
                size_mb=args.size_mb, nc_size_mb=128, records=args.records, ops=args.ops, value_size=args.value_size,
                cpu=cpu, timeout=args.timeout, spec=str(HERE/'workloadb_90read.spec'), ini=str(HERE/'config.ini'))
            worker = Worker(settings, 'local-ncc-'+str(os.getpid()), 'fixture', log)
            workers.append(worker)
            worker.peer_check = guard.check
            result[role+'_pid'] = worker.process.pid
            if worker.receive() != 'READY':
                raise RuntimeError(role+' did not become ready')
            (output/(role+'.maps')).write_text(Path(f'/proc/{worker.process.pid}/maps').read_text())
        b, a = workers
        a.send('run'); b.send('run')
        for role, worker in (('A', a), ('B', b)):
            reply = worker.receive()
            if not reply.startswith('RESULT '):
                raise RuntimeError(role+': '+reply)
            report = json.loads(reply[7:])
            if report['errors'] or report['reads']+report['writes'] != args.ops or report['bwtree_opt'] != bool(args.opt):
                raise RuntimeError(role+': incorrect workload result '+str(report))
            # Retain raw logs, but do not present instrumented timings as throughput.
            result[role] = {k: report[k] for k in ('reads', 'writes', 'errors', 'bwtree_opt', 'rpc_completed', 'trace_hash',
                                                 'verified_keys', 'verified_versions')}
            if args.suite and (not report['verified_keys'] or report['verified_versions'] != 32):
                raise RuntimeError(role+': missing final-state / versioned payload verification')
        if args.suite and result['B']['rpc_completed'] != 0:
            raise RuntimeError('A updates did not all execute on B')
        if result['A']['writes']:
            raise RuntimeError('A unexpectedly wrote CACHE data')
        a.finish('close'); b.finish('release')
        for worker in workers:
            worker.reader.join(timeout=2)
        for role in ('A', 'B'):
            text = (output/(role+'.log')).read_text()
            if 'LOCAL_NCC_MAP' not in text:
                raise RuntimeError('Missing replica backend marker')
            if result['startup_invalidate'] and 'UB_STARTUP_INVALIDATE' not in text:
                raise RuntimeError('Missing startup invalidation on '+role)
            stats = re.search(r'LOCAL_NCC_STATS[^\n]*', text)
            if not stats:
                raise RuntimeError('Missing cache counters on '+role)
            result[role]['cache_stats'] = {k: int(v) for k,v in re.findall(r'(\w+)=(\d+)', stats.group()) if k != 'pid'}
            acquired = re.search(r'UB_COW_ACQUIRE objects=(\d+)', text)
            if not acquired:
                raise RuntimeError('Missing CoW acquisition counters on '+role)
            result[role]['acquired_objects'] = int(acquired[1])
            if int(acquired[1]):
                raise RuntimeError('Unexpected per-object acquisition on '+role)
        if list((output/'objects').iterdir()):
            raise RuntimeError('Objects still mapped after successful cleanup')
        guard.check()
        result['pass'] = True
    except BaseException as error:
        result['error'] = repr(error)
        print('FAIL: '+str(error), flush=True)
    finally:
        if contract_process and contract_process.poll() is None:
            os.killpg(contract_process.pid, signal.SIGKILL)
            contract_process.wait()
        for worker in workers:
            if worker.process.poll() is None:
                worker.process.terminate()
            try:
                worker.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                worker.process.kill(); worker.process.wait()
            worker.reader.join(timeout=2); worker.protocol_reader.join(timeout=2)
        for log in logs:
            log.close()
        guard.close()
        result['interfered'] = guard.conflict.is_set()
        result['exit_codes'] = {w.role: w.process.returncode for w in workers}
        if result['interfered']:
            result['pass'] = False
        (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(('PASS' if result['pass'] else 'FAIL')+': '+str(output/'result.json'))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
