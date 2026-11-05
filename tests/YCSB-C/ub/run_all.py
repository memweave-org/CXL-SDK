#!/usr/bin/env python3
"""Sequential, fail-fast UB comparison using the original two-host YCSB framework."""
import argparse
import csv
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

HERE=Path(__file__).resolve().parent
CASES=('bwtree','bwtree+opt','bwtree+rpc',
       'clevelhash','clevelhash+opt','clevelhash+rpc','sherman+nc-direct','sherman+nc-memcpy')
# Include drivers: a worker finishing or a temporary CPU dip does not end a round.
TEST=re.compile(r'(ycsbc|bench|^run.*\.(sh|py)$|sweep|scale\.py|^test_(framework|run_all)\.py$|^direct$|^prep$)',re.I)


class ResourceGuard:
    def __init__(self, output, fixture):
        self.log=(output/'exclusivity.jsonl').open('a')
        self.fixture=fixture
        self.previous={}
        self.active_conflicts={}

    def snapshot(self):
        now=time.monotonic()
        rows={}
        for path in Path('/proc').glob('[0-9]*'):
            try:
                stat=(path/'stat').read_text().rsplit(')',1)[1].split()
                argv=(path/'cmdline').read_bytes().split(b'\0')
                args=[x.decode(errors='replace') for x in argv if x]
                if not args: continue
                pid=int(path.name); ticks=int(stat[11])+int(stat[12]); start_ticks=int(stat[19])
                old=self.previous.get(pid)
                cpu=0 if old is None or old[2]!=start_ticks else (ticks-old[1])/os.sysconf('SC_CLK_TCK')/(now-old[0])*100
                rows[pid]={'pid':pid,'ppid':int(stat[1]),'cpu_percent':round(cpu,2),'args':args,
                           'ticks':ticks,'start_ticks':start_ticks,'state':stat[0]}
            except (OSError,ValueError,IndexError):
                continue
        self.previous={pid:(now,row['ticks'],row['start_ticks']) for pid,row in rows.items()}
        own={os.getpid()}
        parent=os.getppid()
        ancestors=set()
        while parent in rows and parent not in ancestors:
            ancestors.add(parent); parent=rows[parent]['ppid']
        if self.fixture:
            marker=('UBSM_TEST_DIRECTORY='+os.environ['UBSM_TEST_DIRECTORY']).encode()
            for pid in rows:
                try:
                    if marker in Path(f'/proc/{pid}/environ').read_bytes().split(b'\0'):
                        own.add(pid)
                except OSError: pass
        # Include descendants of the other role in the same explicit local
        # fixture, even if a short-lived worker exits while /proc is sampled.
        while True:
            added={pid for pid,row in rows.items() if row['ppid'] in own}-own
            if not added: break
            own.update(added)
        conflicts=[]
        for pid,row in rows.items():
            if pid in own or pid in ancestors: continue
            # Inspect executable/script names, not arbitrary shell/code strings.
            names=[Path(arg).name for arg in row['args'][:3] if '\n' not in arg and ' ' not in arg]
            if (any(TEST.search(name) for name in names) or row['cpu_percent']>=30 or
                    self.active_conflicts.get(pid)==row['start_ticks']):
                conflicts.append(row)
        exited=[{'pid':pid,'start_ticks':start,'exit_status':'unavailable (not our child)'}
                for pid,start in self.active_conflicts.items()
                if pid not in rows or rows[pid]['start_ticks']!=start]
        self.active_conflicts={row['pid']:row['start_ticks'] for row in conflicts}
        self.log.write(json.dumps({'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'host':socket.gethostname(),'guard_pid':os.getpid(),'owned_pids':sorted(own),
            'owned_processes':[rows[pid] for pid in sorted(own) if pid in rows],
            'loadavg':os.getloadavg(),'conflicts':conflicts,'exited_since_previous':exited})+'\n'); self.log.flush()
        return conflicts

    def await_idle(self):
        clear=0
        while clear<3:
            conflicts=self.snapshot()
            clear=0 if conflicts else clear+1
            if conflicts: print('Waiting for other test/driver/resource users:',conflicts,flush=True)
            if clear<3: time.sleep(2)

    def run(self, command, env, log, watch=True):
        self.await_idle()
        process=subprocess.Popen(command,cwd=HERE,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            while process.poll() is None:
                if watch and self.snapshot():
                    raise RuntimeError('Resource overlap detected; this sample is invalid. Stopping only our process group.')
                time.sleep(0.5)
            if watch and self.snapshot():
                raise RuntimeError('Resource overlap at completion; this sample is invalid.')
            if process.returncode:
                raise RuntimeError(f'Command exited {process.returncode}: {command}; see {log.name}')
        finally:
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL); process.wait()
            self.log.write(json.dumps({'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'command':command,'driver_pid':process.pid,'exit_status':process.returncode})+'\n'); self.log.flush()


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'Received signal {signum}')
    signal.signal(signal.SIGTERM,interrupted)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dry-run',action='store_true',help='Print the eight cases and common workload without building/running')
    p.add_argument('--output',type=Path)
    p.add_argument('--bundles',type=Path,help='Reuse prepared opt/ and noopt/ directories, each with ycsbc and lib/')
    p.add_argument('--build-only',action='store_true')
    p.add_argument('--fixture',action='store_true',help='Explicit local SDK fixture, never a hardware/performance result')
    args=p.parse_args()
    if os.environ.get('A_WRITABLE','0')!='0': p.error('A is always read-only; A_WRITABLE must be 0')
    spec=Path(os.environ['SPEC'])
    if not spec.is_absolute(): spec=HERE/spec
    common={key:os.environ[key] for key in ('A_IP','B_IP','THREADS_A','THREADS_B','RECORDS','OPS','SIZE_MB','NC_SIZE_MB','SEED','A_WRITABLE')}
    plan={'cases':list(CASES),'common':common,'spec':str(spec),'workload':spec.read_text(),
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
    out=(args.output or Path(tempfile.mkdtemp(prefix=f'result-all-{role or "build"}-',dir=HERE))).resolve()
    out.mkdir(parents=True,exist_ok=True)
    if (out/'manifest.json').exists(): p.error('Output already contains a run; use a new directory')
    (out/'manifest.json').write_text(json.dumps(plan,indent=2))
    print(f'UB run_all role={role}; results: {out}',flush=True)
    lock_path=Path(tempfile.gettempdir())/('cxl-sdk-ub-benchmark'+('-fixture-'+str(role) if args.fixture else '')+'.lock')
    lock=os.open(lock_path,os.O_CREAT|os.O_RDWR,0o666)
    if os.fstat(lock).st_uid==os.getuid(): os.fchmod(lock,0o666)
    try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError: p.error(f'Another UB batch holds {lock_path}')
    guard=ResourceGuard(out,args.fixture)
    reports=[]; report={'pass':False,'cases':reports,'fixture':args.fixture}
    stage='build'
    try:
        bundles=(args.bundles or out/'bundles').resolve()
        if not args.bundles:
            for mode,opt in [('noopt','0'),('opt','1')]:
                env=dict(os.environ,OPT=opt,YCSB_INDEXES='bwtree;clevel-hash;sherman')
                with (out/f'build-{mode}.log').open('w') as log:
                    # Docker compiler processes belong to the daemon rather than
                    # this process tree. They are not concurrent test workers.
                    guard.run(['bash','build.sh',str(bundles/mode)],env,log,watch=False)
        for mode in ('noopt','opt'):
            if not (bundles/mode/'ycsbc').is_file() or not (bundles/mode/'lib').is_dir():
                raise RuntimeError(f'Incomplete bundle: {bundles/mode}')
        if not args.build_only:
            baseline=None
            for case in CASES:
                stage=case
                directory=out/case; directory.mkdir()
                mode='opt' if case.endswith('+opt') else 'noopt'
                env=dict(os.environ,YCSB_UB_ROLE=role,YCSB_UB_CASE=case,YCSB_UB_OUTPUT=str(directory),
                         YCSB_UB_SUITE='1',
                         YCSB_UB_BUNDLE=str(bundles/mode),YCSB_UB_FIXTURE=str(int(args.fixture)))
                print(f'[{role}] running {case}',flush=True)
                with (directory/'launcher.log').open('w') as log:
                    guard.run(['bash','run.sh'],env,log)
                result=json.loads((directory/'result.json').read_text())
                if not result.get('pass'): raise RuntimeError(f'{case} failed')
                hashes=[result[host]['trace_hash'] for host in ('A','B')]
                if baseline is not None and hashes!=baseline:
                    raise RuntimeError(f'{case} generated a different workload trace')
                baseline=hashes; reports.append(result)
                print(f'[{role}] PASS {case}: A {result["A"]["reads"]}R/{result["A"]["writes"]}W, B {result["B"]["reads"]}R/{result["B"]["writes"]}W',flush=True)
        report['pass']=True
    except (Exception,KeyboardInterrupt) as error:
        report.update(error=str(error),stage=stage)
        if stage in CASES:
            (out/stage/'invalid.json').write_text(json.dumps({'valid':False,'error':str(error)},indent=2))
        print(f'FAILED at {stage}: {error}',flush=True)
    finally:
        (out/'summary.json').write_text(json.dumps(report,indent=2))
        with (out/'summary.csv').open('w',newline='') as stream:
            writer=csv.writer(stream)
            writer.writerow(['case','pass','opt','sherman_access','cache_policy','A_reads','A_writes','B_reads','B_writes','rpc_completed','seconds','total_mops','fixture'])
            for r in reports:
                writer.writerow([r['case'],r['pass'],r['bwtree_opt'],r.get('sherman_access'),r['cache_policy'],r['A']['reads'],r['A']['writes'],r['B']['reads'],r['B']['writes'],
                                 r['B']['rpc_completed'],r['common_seconds'],r['total_mops'],args.fixture])
        guard.log.close(); os.close(lock)
    return 0 if report['pass'] else 1

if __name__=='__main__':
    raise SystemExit(main())
