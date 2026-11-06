#!/usr/bin/env python3
"""Run A-read-only BwTree workloads, including RPC, at equal worker counts."""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import socket
from contextlib import ExitStack

from run_all import run_command, run_commands
from cpu_affinity import select_cpus, cxl_cpu_nodes, select_cxl_role_cpus

HERE = Path(__file__).resolve().parent
def check_cpus(counts, first, allowed):
    return select_cpus(max(counts), first, allowed)


def validate_result(result, opt, rpc, ops):
    if (not result.get('pass') or result['A']['writes'] != 0 or
            any(result[r]['bwtree_opt'] != bool(opt) for r in ('A', 'B')) or
            bool(result.get('rpc', False)) != rpc):
        raise RuntimeError('Failed workload, wrong OPT mode, or wrong RPC mode')
    for role, peer in (('A', 'B'), ('B', 'A')):
        local, remote = result[role], result[peer]
        if rpc:
            sent = local.get('rpc_sent')
            if (sent is None or sent != local.get('rpc_expected_remote') or
                    sent != remote.get('rpc_completed') or
                    local.get('rpc_local', -1) + sent != ops or
                    local.get('index_storage') != 'local-dram' or
                    local.get('rpc_transport') != 'original-mq-thread-pool'):
                raise RuntimeError('Partitioned RPC routing/completion/storage mismatch')
        elif local.get('rpc_completed', 0) != 0:
            raise RuntimeError('Unexpected RPC completion in direct mode')


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
    parser.add_argument('--thread-counts', nargs='+', type=int)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--bundle', type=Path, help='Reuse an already built BwTree bundle')
    parser.add_argument('--backend', choices=['ub','cxl'], default=os.environ.get('YCSB_BACKEND','ub'))
    parser.add_argument('--cxl-node', type=int, default=int(os.environ.get('CXL_NODE','4')))
    args = parser.parse_args(argv)
    try:
        default_counts = ('24' if os.environ.get('YCSB_UB_CASE')=='bwtree+rpc' else '32') if args.backend=='cxl' else '32 64 128'
        counts = [int(n) for n in (args.thread_counts or os.environ.get('THREAD_COUNTS',default_counts).split())]
        if not counts or any(n < 1 or n > 192 for n in counts) or len(set(counts)) != len(counts):
            raise ValueError('Thread counts must be distinct integers in 1..192')
        case = os.environ.get('YCSB_UB_CASE', 'bwtree')
        if case not in ('bwtree', 'bwtree+rpc'):
            raise ValueError('The BwTree thread sweep supports bwtree or bwtree+rpc')
        rpc = case == 'bwtree+rpc'
        opt = 0 if rpc else int(os.environ.get('OPT', '1'))
        if opt not in (0, 1):
            raise ValueError('OPT must be 0 or 1')
        if os.environ.get('A_WRITABLE', '0') != '0':
            raise ValueError('A is always read-only; A_WRITABLE must be 0')
        if os.environ.get('YCSB_UB_FIXTURE', '0') != '0':
            raise ValueError('The thread sweep requires two hardware hosts; use the fixture test runners separately')
        spec = Path(os.environ['SPEC'])
        if not spec.is_absolute():
            spec = HERE / spec
        plan = {'case': case, 'opt': bool(opt), 'suite': rpc, 'rpc': rpc,
                'build_entry': '../build.sh', 'variant': 'cc_nocc_mq' if rpc else 'nocc' if opt else 'nocc_no_opt',
                'rpc_scheduler': 'original ThreadPool' if rpc else None,
                'rpc_services_per_host': 'same as client count' if rpc else 0,
                'backend': args.backend, 'cxl_node': args.cxl_node if args.backend=='cxl' else None,
                'A_workload': '100% read', 'B_workload': '90% read / 10% update',
                'update_semantics': 'replace-existing-key' if rpc else 'fresh-insert',
                'index_storage': 'local-dram' if rpc else 'shared-cxl' if args.backend=='cxl' else 'shared-ub',
                'rpc_transport': ('cxl-shared-queue' if args.backend=='cxl' else 'ub-nc-shared-queue') if rpc else 'none',
                'records': int(os.environ['RECORDS']),
                'operations_per_host': int(os.environ['OPS']),
                'rounds': [{'threads_A': n, 'threads_B': n} for n in counts],
                'spec': str(spec), 'workload': spec.read_text()}
        if args.backend=='cxl':
            plan['cpu_numa_nodes']=cxl_cpu_nodes(os.environ)
        if args.dry_run:
            print(json.dumps(plan, indent=2))
            return 0
        role = identify_role() if args.backend=='ub' else 'local'
        allowed = os.sched_getaffinity(0)
        if args.backend=='cxl':
            if args.cxl_node<0 or not Path(f'/sys/devices/system/node/node{args.cxl_node}').exists():
                raise ValueError('Requested CXL NUMA node does not exist')
            role_cpus=select_cxl_role_cpus(max(counts)*(2 if rpc else 1),allowed,os.environ)
            cpu=role_cpus['B'][0]; cpus=role_cpus
        else:
            cpu = int(os.environ.get('CPU_' + role) or min(allowed))
            cpus = check_cpus([n*(2 if rpc else 1) for n in counts], cpu, allowed)
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))

    output = args.output or os.environ.get('YCSB_UB_OUTPUT')
    out = Path(output or tempfile.mkdtemp(prefix=f'result-threads-{role}-', dir=HERE)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error('Output directory must be empty; previous samples must not be overwritten')
    (out / 'manifest.json').write_text(json.dumps({**plan, 'role': role, 'cpu_start': cpu,
                                                 'worker_cpus': cpus}, indent=2))
    print(f'[{role}] backend={args.backend}, case={case}, OPT={opt}, RPC={int(rpc)}, A reads only; per-role workers={counts}; output={out}', flush=True)
    print(f'[{role}] Allowed logical CPUs={len(allowed)}; selected CPU IDs={cpus}', flush=True)
    report = {'pass': False, 'rounds': []}
    stage = 'build'
    directory = None
    try:
        bundle = args.bundle or os.environ.get('YCSB_UB_BUNDLE')
        if bundle:
            bundle = Path(bundle).resolve()
            print(f'[{role}] Reusing bundle: {bundle}',flush=True)
        else:
            bundle = out / 'bundle'
            print(f'[{role}] Building {args.backend} BwTree OPT={opt}; log={out / "build.log"}',flush=True)
            with (out / 'build.log').open('w') as log:
                run_command(['bash', 'build.sh', str(bundle)],
                            dict(os.environ, OPT=str(opt), YCSB_RPC_BUILD=str(int(rpc)), YCSB_INDEXES='bwtree',YCSB_BACKEND=args.backend), log)
        if not (bundle / 'ycsbc').is_file() or not (bundle / 'lib').is_dir():
            raise RuntimeError(f'Incomplete bundle: {bundle}')
        for n in counts:
            stage = f'threads-{n}'
            directory = out / stage
            directory.mkdir()
            # peer.py includes the complete sweep and this round in its handshake.
            # It returns only after A has unmapped and B has released the object.
            env = dict(os.environ, YCSB_UB_ROLE=role, YCSB_UB_CASE=case,
                       YCSB_UB_SUITE=str(int(rpc)), YCSB_UB_OUTPUT=str(directory),
                       YCSB_UB_BUNDLE=str(bundle), YCSB_UB_FIXTURE='0', OPT=str(opt),
                       THREADS_A=str(n), THREADS_B=str(n), A_WRITABLE='0',
                       YCSB_UB_THREAD_SWEEP=json.dumps({'counts': counts, 'threads': n}),
                       YCSB_BACKEND=args.backend)
            env['CPU_' + role] = str(cpu)
            print(f'[{role}] Preparing {n}-worker A/B round; workers start after peer coordination; output={directory}', flush=True)
            if args.backend=='cxl':
                backing=Path(tempfile.mkdtemp(prefix='ycsb-cxl-',dir='/dev/shm'))
                with socket.socket() as port_socket:
                    port_socket.bind(('127.0.0.1',0)); port=port_socket.getsockname()[1]
                (directory/'cxl.json').write_text(json.dumps({'backing_directory':str(backing),
                    'numa_node':args.cxl_node,'cpu_numa_nodes':plan['cpu_numa_nodes'],
                    'worker_cpus':{r:role_cpus[r][:n] for r in ('A','B')}},indent=2))
                with ExitStack() as stack:
                    commands=[]
                    for r in ('B','A'):
                        local=directory/r; local.mkdir()
                        local_env=dict(env,YCSB_UB_ROLE=r,YCSB_UB_OUTPUT=str(local),
                            B_IP='127.0.0.1',PORT=str(port),PROVIDER_B='local-cxl',
                            YCSB_CXL_DIRECTORY=str(backing),YCSB_CXL_NODE=str(args.cxl_node))
                        local_env['CPU_'+r]=str(role_cpus[r][0])
                        log=stack.enter_context((local/'launcher.log').open('w'))
                        commands.append((['taskset','-c',','.join(map(str,role_cpus[r])),
                                          'bash','run.sh'],local_env,log))
                    run_commands(commands)
                result=json.loads((directory/'A'/'result.json').read_text())
                (directory/'result.json').write_text(json.dumps(result,indent=2))
                backing.rmdir() # B removes both objects only after A has unmapped.
            else:
                with (directory / 'launcher.log').open('w') as log:
                    run_command(['bash', 'run.sh'], env, log)
                result = json.loads((directory / 'result.json').read_text())
            validate_result(result, opt, rpc, plan['operations_per_host'])
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
