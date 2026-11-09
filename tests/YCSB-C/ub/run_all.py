#!/usr/bin/env python3
"""Sequential, fail-fast CXL/UB comparison using the original two-host YCSB framework."""
import argparse
import csv
from contextlib import ExitStack
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

from cpu_affinity import select_cpus, cxl_cpu_nodes, select_cxl_role_cpus

HERE=Path(__file__).resolve().parent
CASES=('bwtree','bwtree+opt','bwtree+rpc',
       'clevelhash','clevelhash+opt','clevelhash+rpc','sherman+nc-direct','sherman+nc-memcpy')


def run_command(command, env, log):
    run_commands([(command,env,log)])


def run_commands(commands):
    processes=[]
    readers=[]
    def output(process,log):
        with process.stdout:
            for line in process.stdout:
                log.write(line); log.flush()
                print(line,end='',flush=True)
    try:
        for command,env,log in commands:
            process=subprocess.Popen(command,cwd=HERE,env=env,stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,text=True,errors='replace',bufsize=1,start_new_session=True)
            processes.append((process,command,log))
            reader=threading.Thread(target=output,args=(process,log),daemon=True)
            reader.start(); readers.append(reader)
        while True:
            statuses=[process.poll() for process,_,_ in processes]
            for status,(_,command,log) in zip(statuses,processes):
                if status not in (None,0):
                    raise RuntimeError(f'Command exited {status}: {command}; see {log.name}')
            if all(status is not None for status in statuses): break
            time.sleep(0.1)
    finally:
        for process,_,_ in processes:
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL); process.wait()
        for reader in readers: reader.join(timeout=3)


def main(argv=None):
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'Received signal {signum}')
    signal.signal(signal.SIGTERM,interrupted)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dry-run',action='store_true',help='Print the case/worker matrix without building/running')
    p.add_argument('--backend', choices=('ub','cxl'), default=os.environ.get('YCSB_BACKEND','ub'))
    p.add_argument('--cxl-node', type=int, default=int(os.environ.get('CXL_NODE','4')))
    p.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    p.add_argument('--thread-counts',nargs='+',type=int)
    p.add_argument('--output',type=Path)
    p.add_argument('--bundles',type=Path,help='Reuse prepared opt/, noopt/ and rpc/ directories, each with ycsbc and lib/')
    p.add_argument('--build-only',action='store_true')
    p.add_argument('--fixture',action='store_true',help='Explicit local SDK fixture, never a hardware/performance result')
    args=p.parse_args(argv)
    try:
        default_counts=('24' if any(c.endswith('+rpc') for c in args.cases) else '32') if args.backend=='cxl' else '32 64 128'
        counts=[int(n) for n in (args.thread_counts or os.environ.get('THREAD_COUNTS',default_counts).split())]
        if not counts or any(n<1 or n>192 for n in counts) or len(set(counts))!=len(counts):
            raise ValueError('Thread counts must be distinct integers in 1..192')
    except ValueError as error:
        p.error(str(error))
    rounds=[{'case':case,'threads_A':n,'threads_B':n} for case in args.cases for n in counts]
    if os.environ.get('A_WRITABLE','0')!='0': p.error('A is always read-only; A_WRITABLE must be 0')
    spec=Path(os.environ['SPEC'])
    if not spec.is_absolute(): spec=HERE/spec
    common={key:os.environ[key] for key in ('A_IP','B_IP','RECORDS','OPS','SIZE_MB','NC_SIZE_MB','SEED','A_WRITABLE')}
    memory_by_case={case:{
        'cache_mb':0 if case.startswith('sherman+nc-') or case.endswith('+rpc') else int(common['SIZE_MB']),
        'nc_mb':int(os.environ.get('SHERMAN_NC_SIZE_MB',common['NC_SIZE_MB'])
                    if case.startswith('sherman+nc-') else common['NC_SIZE_MB'])}
        for case in args.cases}
    plan={'backend':args.backend,'process_model':'A/B independent processes','cases':args.cases,'thread_counts':counts,'rounds':rounds,'common':common,'spec':str(spec),'workload':spec.read_text(),
          'memory_by_case':memory_by_case,
          'rpc_local_mb_per_process':int(common['SIZE_MB']),
          'providers':{'cache':os.environ.get('PROVIDER_CACHE') or os.environ.get('PROVIDER_B') or 'B hostname',
                       'nc':os.environ.get('PROVIDER_NC') or os.environ.get('PROVIDER_B') or 'B hostname'},
          'A_workload':'100% read','B_workload':'90% read / 10% update',
          'update_semantics':'replace-existing-key','fixture':args.fixture}
    plan['sherman']={'mapping':'NC only','writers':'B only, no RPC',
                     'modes':['in-place loads/stores','local memcpy and copy-back']}
    if args.backend=='cxl':
        plan.update(cpu_numa_nodes=cxl_cpu_nodes(os.environ),cxl_node=args.cxl_node)
    if args.dry_run:
        print(json.dumps(plan,indent=2)); return 0
    if args.fixture and (not args.bundles or not os.environ.get('UBSM_TEST_DIRECTORY')):
        p.error('--fixture requires --bundles and UBSM_TEST_DIRECTORY')
    if args.backend=='cxl' and args.fixture: p.error('CXL uses its native mapping backend; --fixture is UB only')
    role='local' if args.backend=='cxl' else os.environ.get('YCSB_UB_ROLE')
    if not role and not args.build_only:
        interfaces=json.loads(subprocess.check_output(['ip','-j','-4','addr','show'],text=True))
        ips={a['local'] for i in interfaces for a in i.get('addr_info',[])}
        roles=[r for r in ('A','B') if os.environ[r+'_IP'] in ips]
        if len(roles)!=1: p.error('Cannot identify A/B from local IP; check config.sh')
        role=roles[0]
    if role not in ('A','B','local',None): p.error('YCSB_UB_ROLE must be A or B')
    if not args.build_only:
        try:
            allowed=os.sched_getaffinity(0)
            service_factor=2 if any(c.endswith('+rpc') for c in args.cases) else 1
            needed=service_factor*max(counts)
            if args.backend=='cxl':
                if args.cxl_node<0 or not Path(f'/sys/devices/system/node/node{args.cxl_node}').exists():
                    raise ValueError('Requested CXL NUMA node does not exist')
                role_cpus=select_cxl_role_cpus(needed,allowed,os.environ)
                cpu=role_cpus['B'][0]; cpus=role_cpus
            else:
                cpu=int(os.environ.get('CPU_'+role) or min(allowed))
                cpus=select_cpus(needed,cpu,allowed)
            plan.update(role=role,cpu_start=cpu,worker_and_service_cpus=cpus)
        except ValueError as error:
            p.error(str(error))
    out=(args.output or Path(tempfile.mkdtemp(prefix=f'result-all-{role or "build"}-',dir=HERE))).resolve()
    out.mkdir(parents=True,exist_ok=True)
    if (out/'manifest.json').exists(): p.error('Output already contains a run; use a new directory')
    (out/'manifest.json').write_text(json.dumps(plan,indent=2))
    print(f'{args.backend.upper()} run_all role={role}; results: {out}',flush=True)
    reports=[]; report={'pass':False,'cases':reports,'thread_counts':counts,'fixture':args.fixture,'backend':args.backend}
    stage='build'
    directory=None
    try:
        bundles=(args.bundles or out/'bundles').resolve()
        cases_by_mode={mode:[case for case in args.cases
                            if ('rpc' if case.endswith('+rpc') else
                                'opt' if case.endswith('+opt') else 'noopt')==mode]
                       for mode in ('noopt','opt','rpc')}
        cases_by_mode={mode:cases for mode,cases in cases_by_mode.items() if cases}
        if not args.bundles:
            for mode,cases in cases_by_mode.items():
                indexes=list(dict.fromkeys('clevel-hash' if case.startswith('clevelhash')
                                           else case.split('+')[0] for case in cases))
                env=dict(os.environ,YCSB_BACKEND=args.backend,OPT=str(int(mode=='opt')),YCSB_RPC_BUILD=str(int(mode=='rpc')),YCSB_INDEXES=';'.join(indexes))
                with (out/f'build-{mode}.log').open('w') as log:
                    run_command(['bash','build.sh',str(bundles/mode)],env,log)
        for mode in cases_by_mode:
            if not (bundles/mode/'ycsbc').is_file() or not (bundles/mode/'lib').is_dir():
                raise RuntimeError(f'Incomplete bundle: {bundles/mode}')
        if not args.build_only:
            baselines={}
            for round_ in rounds:
                case=round_['case']; n=round_['threads_A']
                stage=f'{case}/threads-{n}'
                directory=out/stage; directory.mkdir(parents=True)
                mode='rpc' if case.endswith('+rpc') else 'opt' if case.endswith('+opt') else 'noopt'
                env=dict(os.environ,YCSB_BACKEND=args.backend,YCSB_UB_ROLE=role,YCSB_UB_CASE=case,YCSB_UB_OUTPUT=str(directory),
                         NC_SIZE_MB=str(memory_by_case[case]['nc_mb']),
                         YCSB_UB_SUITE='1',THREADS_A=str(n),THREADS_B=str(n),
                         YCSB_UB_THREAD_SWEEP=json.dumps({'runner':'run_all','cases':args.cases,
                             'counts':counts,'case':case,'threads':n}),
                         YCSB_UB_BUNDLE=str(bundles/mode),YCSB_UB_FIXTURE=str(int(args.fixture)))
                env['CPU_'+role]=str(cpu)
                print(f'[{role}] running {case}, {n} workers per host',flush=True)
                if args.backend=='cxl':
                    backing=Path(tempfile.mkdtemp(prefix='ycsb-cxl-all-',dir='/dev/shm'))
                    with socket.socket() as sock:
                        sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
                    (directory/'cxl.json').write_text(json.dumps({'backing_directory':str(backing),
                        'numa_node':args.cxl_node,'cpu_numa_nodes':plan['cpu_numa_nodes'],'cpu_sets':role_cpus},indent=2))
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
                    result=json.loads((directory/'A/result.json').read_text())
                    backing.rmdir() # B deletes its objects only after A unmaps.
                else:
                    with (directory/'launcher.log').open('w') as log:
                        run_command(['bash','run.sh'],env,log)
                    result=json.loads((directory/'result.json').read_text())
                if not result.get('pass'): raise RuntimeError(f'{stage} failed')
                hashes=[result[host]['trace_hash'] for host in ('A','B')]
                if n in baselines and hashes!=baselines[n]:
                    raise RuntimeError(f'{stage} generated a different workload trace for {n} workers')
                baselines[n]=hashes
                result.update(threads_A=n,threads_B=n,memory=memory_by_case[case])
                (directory/'result.json').write_text(json.dumps(result,indent=2))
                reports.append(result)
                print(f'[{role}] PASS {stage}: A {result["A"]["reads"]}R/{result["A"]["writes"]}W, B {result["B"]["reads"]}R/{result["B"]["writes"]}W',flush=True)
                directory=None
        report['pass']=True
    except (Exception,KeyboardInterrupt) as error:
        report.update(error=str(error),stage=stage)
        if directory is not None:
            (directory/'invalid.json').write_text(json.dumps({'valid':False,'error':str(error)},indent=2))
        print(f'FAILED at {stage}: {error}',flush=True)
    finally:
        (out/'summary.json').write_text(json.dumps(report,indent=2))
        with (out/'summary.csv').open('w',newline='') as stream:
            writer=csv.writer(stream)
            writer.writerow(['case','threads_A','threads_B','pass','opt','sherman_access','cache_policy','A_reads','A_writes','B_reads','B_writes','rpc_completed','seconds','total_mops','fixture'])
            for r in reports:
                writer.writerow([r['case'],r['threads_A'],r['threads_B'],r['pass'],r['bwtree_opt'],r.get('sherman_access'),r['cache_policy'],r['A']['reads'],r['A']['writes'],r['B']['reads'],r['B']['writes'],
                                 r['B']['rpc_completed'],r['common_seconds'],r['total_mops'],args.fixture])
    return 0 if report['pass'] else 1

if __name__=='__main__':
    raise SystemExit(main())
