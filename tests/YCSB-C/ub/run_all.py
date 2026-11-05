#!/usr/bin/env python3
"""Sequential, fail-fast UB comparison using the original two-host YCSB framework."""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import time

from cpu_affinity import select_cpus

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
    p.add_argument('--thread-counts',nargs='+',type=int,default=os.environ.get('THREAD_COUNTS','32 64 128').split())
    p.add_argument('--output',type=Path)
    p.add_argument('--bundles',type=Path,help='Reuse prepared opt/ and noopt/ directories, each with ycsbc and lib/')
    p.add_argument('--build-only',action='store_true')
    p.add_argument('--fixture',action='store_true',help='Explicit local SDK fixture, never a hardware/performance result')
    args=p.parse_args(argv)
    try:
        counts=[int(n) for n in args.thread_counts]
        if not counts or any(n<1 or n>192 for n in counts) or len(set(counts))!=len(counts):
            raise ValueError('Thread counts must be distinct integers in 1..192')
    except ValueError as error:
        p.error(str(error))
    rounds=[{'case':case,'threads_A':n,'threads_B':n} for case in CASES for n in counts]
    if os.environ.get('A_WRITABLE','0')!='0': p.error('A is always read-only; A_WRITABLE must be 0')
    spec=Path(os.environ['SPEC'])
    if not spec.is_absolute(): spec=HERE/spec
    common={key:os.environ[key] for key in ('A_IP','B_IP','RECORDS','OPS','SIZE_MB','NC_SIZE_MB','SEED','A_WRITABLE')}
    memory_by_case={case:{
        'cache_mb':0 if case.startswith('sherman+nc-') or case.endswith('+rpc') else int(common['SIZE_MB']),
        'nc_mb':int(os.environ.get('SHERMAN_NC_SIZE_MB',common['NC_SIZE_MB'])
                    if case.startswith('sherman+nc-') else common['NC_SIZE_MB'])}
        for case in CASES}
    plan={'cases':list(CASES),'thread_counts':counts,'rounds':rounds,'common':common,'spec':str(spec),'workload':spec.read_text(),
          'memory_by_case':memory_by_case,
          'providers':{'cache':os.environ.get('PROVIDER_CACHE') or os.environ.get('PROVIDER_B') or 'B hostname',
                       'nc':os.environ.get('PROVIDER_NC') or os.environ.get('PROVIDER_B') or 'B hostname'},
          'A_workload':'100% read','B_workload':'90% read / 10% update',
          'update_semantics':'replace-existing-key','fixture':args.fixture}
    plan['sherman']={'mapping':'NC only','writers':'B only, no RPC',
                     'modes':['in-place loads/stores','local memcpy and copy-back']}
    if args.dry_run:
        print(json.dumps(plan,indent=2)); return 0
    if args.fixture and (not args.bundles or not os.environ.get('UBSM_TEST_DIRECTORY')):
        p.error('--fixture requires --bundles and UBSM_TEST_DIRECTORY')
    role=os.environ.get('YCSB_UB_ROLE')
    if not role and not args.build_only:
        interfaces=json.loads(subprocess.check_output(['ip','-j','-4','addr','show'],text=True))
        ips={a['local'] for i in interfaces for a in i.get('addr_info',[])}
        roles=[r for r in ('A','B') if os.environ[r+'_IP'] in ips]
        if len(roles)!=1: p.error('Cannot identify A/B from local IP; check config.sh')
        role=roles[0]
    if role not in ('A','B',None): p.error('YCSB_UB_ROLE must be A or B')
    if not args.build_only:
        try:
            allowed=os.sched_getaffinity(0)
            cpu=int(os.environ.get('CPU_'+role) or min(allowed))
            cpus=select_cpus(max(counts),cpu,allowed)
            plan.update(role=role,cpu_start=cpu,worker_cpus=cpus)
        except ValueError as error:
            p.error(str(error))
    out=(args.output or Path(tempfile.mkdtemp(prefix=f'result-all-{role or "build"}-',dir=HERE))).resolve()
    out.mkdir(parents=True,exist_ok=True)
    if (out/'manifest.json').exists(): p.error('Output already contains a run; use a new directory')
    (out/'manifest.json').write_text(json.dumps(plan,indent=2))
    print(f'UB run_all role={role}; results: {out}',flush=True)
    reports=[]; report={'pass':False,'cases':reports,'thread_counts':counts,'fixture':args.fixture}
    stage='build'
    directory=None
    try:
        bundles=(args.bundles or out/'bundles').resolve()
        if not args.bundles:
            for mode,opt in [('noopt','0'),('opt','1')]:
                env=dict(os.environ,OPT=opt,YCSB_INDEXES='bwtree;clevel-hash;sherman')
                with (out/f'build-{mode}.log').open('w') as log:
                    run_command(['bash','build.sh',str(bundles/mode)],env,log)
        for mode in ('noopt','opt'):
            if not (bundles/mode/'ycsbc').is_file() or not (bundles/mode/'lib').is_dir():
                raise RuntimeError(f'Incomplete bundle: {bundles/mode}')
        if not args.build_only:
            baselines={}
            for round_ in rounds:
                case=round_['case']; n=round_['threads_A']
                stage=f'{case}/threads-{n}'
                directory=out/stage; directory.mkdir(parents=True)
                mode='opt' if case.endswith('+opt') else 'noopt'
                env=dict(os.environ,YCSB_UB_ROLE=role,YCSB_UB_CASE=case,YCSB_UB_OUTPUT=str(directory),
                         NC_SIZE_MB=str(memory_by_case[case]['nc_mb']),
                         YCSB_UB_SUITE='1',THREADS_A=str(n),THREADS_B=str(n),
                         YCSB_UB_THREAD_SWEEP=json.dumps({'runner':'run_all','cases':list(CASES),
                             'counts':counts,'case':case,'threads':n}),
                         YCSB_UB_BUNDLE=str(bundles/mode),YCSB_UB_FIXTURE=str(int(args.fixture)))
                env['CPU_'+role]=str(cpu)
                print(f'[{role}] running {case}, {n} workers per host',flush=True)
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
