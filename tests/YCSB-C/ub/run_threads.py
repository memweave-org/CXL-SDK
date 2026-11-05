#!/usr/bin/env python3
"""Run the original A-read-only BwTree workload at equal per-host worker counts."""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile

from run_all import run_command
from cpu_affinity import select_cpus

HERE = Path(__file__).resolve().parent
def check_cpus(counts, first, allowed):
    return select_cpus(max(counts), first, allowed)


def identify_role():
    role = os.environ.get('YCSB_UB_ROLE')
    if not role:
        interfaces = json.loads(subprocess.check_output(['ip', '-j', '-4', 'addr', 'show'], text=True))
        addresses = {a['local'] for item in interfaces for a in item.get('addr_info', [])}
        roles = [r for r in ('A', 'B') if os.environ[r + '_IP'] in addresses]
        if len(roles) != 1:
            raise ValueError('Cannot identify A/B from local IP; check ub/config.sh')
        role = roles[0]
    if role not in ('A', 'B'):
        raise ValueError('YCSB_UB_ROLE must be A or B')
    return role


def main(argv=None):
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'Received signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--thread-counts', nargs='+', type=int,
                        default=os.environ.get('THREAD_COUNTS', '32 64 128').split())
    parser.add_argument('--output', type=Path)
    parser.add_argument('--bundle', type=Path, help='Reuse an already built BwTree bundle')
    args = parser.parse_args(argv)
    try:
        counts = [int(n) for n in args.thread_counts]
        if not counts or any(n < 1 or n > 192 for n in counts) or len(set(counts)) != len(counts):
            raise ValueError('Thread counts must be distinct integers in 1..192')
        opt = int(os.environ.get('OPT', '1'))
        if opt not in (0, 1):
            raise ValueError('OPT must be 0 or 1')
        if os.environ.get('A_WRITABLE', '0') != '0':
            raise ValueError('A is always read-only; A_WRITABLE must be 0')
        if os.environ.get('YCSB_UB_FIXTURE', '0') != '0':
            raise ValueError('The thread sweep requires two hardware hosts; use the fixture test runners separately')
        spec = Path(os.environ['SPEC'])
        if not spec.is_absolute():
            spec = HERE / spec
        plan = {'case': 'bwtree', 'opt': bool(opt), 'suite': False, 'rpc': False,
                'A_workload': '100% read', 'B_workload': '90% read / 10% update',
                'update_semantics': 'fresh-insert', 'records': int(os.environ['RECORDS']),
                'operations_per_host': int(os.environ['OPS']),
                'rounds': [{'threads_A': n, 'threads_B': n} for n in counts],
                'spec': str(spec), 'workload': spec.read_text()}
        if args.dry_run:
            print(json.dumps(plan, indent=2))
            return 0
        role = identify_role()
        allowed = os.sched_getaffinity(0)
        cpu = int(os.environ.get('CPU_' + role) or min(allowed))
        cpus = check_cpus(counts, cpu, allowed)
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))

    output = args.output or os.environ.get('YCSB_UB_OUTPUT')
    out = Path(output or tempfile.mkdtemp(prefix=f'result-threads-{role}-', dir=HERE)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error('Output directory must be empty; previous samples must not be overwritten')
    (out / 'manifest.json').write_text(json.dumps({**plan, 'role': role, 'cpu_start': cpu,
                                                 'worker_cpus': cpus}, indent=2))
    print(f'[{role}] BwTree OPT={opt}, A reads only, no RPC; per-host workers={counts}; output={out}', flush=True)
    print(f'[{role}] Allowed logical CPUs={len(allowed)}; selected CPU IDs={cpus}', flush=True)
    report = {'pass': False, 'rounds': []}
    stage = 'build'
    directory = None
    try:
        bundle = args.bundle or os.environ.get('YCSB_UB_BUNDLE')
        if bundle:
            bundle = Path(bundle).resolve()
        else:
            bundle = out / 'bundle'
            with (out / 'build.log').open('w') as log:
                run_command(['bash', 'build.sh', str(bundle)],
                            dict(os.environ, OPT=str(opt), YCSB_INDEXES='bwtree'), log)
        if not (bundle / 'ycsbc').is_file() or not (bundle / 'lib').is_dir():
            raise RuntimeError(f'Incomplete bundle: {bundle}')
        for n in counts:
            stage = f'threads-{n}'
            directory = out / stage
            directory.mkdir()
            # peer.py includes the complete sweep and this round in its handshake.
            # It returns only after A has unmapped and B has released the object.
            env = dict(os.environ, YCSB_UB_ROLE=role, YCSB_UB_CASE='bwtree',
                       YCSB_UB_SUITE='0', YCSB_UB_OUTPUT=str(directory),
                       YCSB_UB_BUNDLE=str(bundle), YCSB_UB_FIXTURE='0', OPT=str(opt),
                       THREADS_A=str(n), THREADS_B=str(n), A_WRITABLE='0',
                       YCSB_UB_THREAD_SWEEP=json.dumps({'counts': counts, 'threads': n}))
            env['CPU_' + role] = str(cpu)
            print(f'[{role}] Starting {n} workers per host; log={directory / "launcher.log"}', flush=True)
            with (directory / 'launcher.log').open('w') as log:
                run_command(['bash', 'run.sh'], env, log)
            result = json.loads((directory / 'result.json').read_text())
            if (not result.get('pass') or result['A']['writes'] != 0 or
                    result['A']['bwtree_opt'] != bool(opt) or result['B']['bwtree_opt'] != bool(opt) or
                    any(result[r].get('rpc_completed', 0) != 0 for r in ('A', 'B'))):
                raise RuntimeError(f'{stage}: failed workload, wrong OPT mode, or unexpected RPC')
            report['rounds'].append({'threads_A': n, 'threads_B': n, 'result': result})
            print(f'[{role}] Finished {n} workers per host: {result["common_seconds"]:.6f}s, '
                  f'{result["total_mops"]:.6f} Mops/s; both hosts cleaned up', flush=True)
            directory = None
        report['pass'] = True
    except (Exception, KeyboardInterrupt) as error:
        report.update(stage=stage, error=str(error))
        if directory is not None:
            (directory / 'invalid.json').write_text(json.dumps({'valid': False, 'error': str(error)}, indent=2))
        print(f'[{role}] FAILED at {stage}: {error}', flush=True)
    finally:
        (out / 'summary.json').write_text(json.dumps(report, indent=2))
        with (out / 'summary.csv').open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(['threads_A', 'threads_B', 'seconds', 'total_mops', 'A_reads', 'B_reads', 'B_writes'])
            for row in report['rounds']:
                r = row['result']
                writer.writerow([row['threads_A'], row['threads_B'], r['common_seconds'],
                                 r['total_mops'], r['A']['reads'], r['B']['reads'], r['B']['writes']])
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
