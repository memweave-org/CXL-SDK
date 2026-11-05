#!/usr/bin/env python3
"""Launch the original ycsbc executable on each host; B is its master, A its follower."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import threading
import time
import uuid
from cpu_affinity import select_cpus


class PeerFailure(RuntimeError):
    def __init__(self, details):
        self.details = details
        super().__init__(f"peer {details.get('role')} failed at {details.get('stage')}: {details.get('error')}")


class PeerChannel:
    """Continuously consume peer messages even while the main thread awaits a worker."""
    def __init__(self, connection, timeout):
        self.connection = connection
        self.stream = connection.makefile('rwb')
        self.timeout = timeout
        self.messages = queue.Queue()
        self.failure = None
        self.closing = threading.Event()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while not self.closing.is_set():
                data = self.stream.readline(65536)
                if not data:
                    raise RuntimeError('peer disconnected without an error report')
                message = json.loads(data)
                if isinstance(message, dict) and message.get('type') == 'ERROR':
                    raise PeerFailure(message)
                self.messages.put(message)
        except Exception as error:
            if not self.closing.is_set():
                self.failure = error

    def check(self):
        if self.failure is not None:
            raise self.failure

    def send(self, message):
        self.stream.write((json.dumps(message)+'\n').encode())
        self.stream.flush()

    def receive(self, check_worker=lambda: None):
        deadline = time.monotonic()+self.timeout
        while True:
            # Deliver a final report already received before normal peer shutdown.
            try:
                return self.messages.get_nowait()
            except queue.Empty:
                pass
            self.check()
            check_worker()
            if time.monotonic() >= deadline:
                raise TimeoutError('peer response timed out')
            try:
                return self.messages.get(timeout=0.1)
            except queue.Empty:
                pass

    def close(self):
        self.closing.set()
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.reader.join(timeout=1)
        try:
            self.stream.close()
        except OSError:
            pass


class Worker:
    def __init__(self, args, name, provider, log):
        self.queue = queue.Queue()
        self.role = args.role
        self.binary = args.binary
        self.data_size = args.size_mb << 20
        self.nc_size = args.nc_size_mb << 20
        self.a_writable = bool(getattr(args,'a_writable',0))
        self.last_stage = 'process launch'
        self.peer_check = lambda: None
        self.finished = False
        self.log_tail = deque(maxlen=12)
        env=dict(os.environ, YCSB_UB_NAME=name, YCSB_UB_PROVIDER=provider,
                 YCSB_UB_THREADS_A=str(args.threads_a), YCSB_UB_THREADS_B=str(args.threads_b),
                 YCSB_UB_SIZE_MB=str(args.size_mb), YCSB_UB_NC_SIZE_MB=str(args.nc_size_mb),
                 YCSB_UB_RECORDS=str(args.records), YCSB_UB_OPS=str(args.ops), YCSB_UB_CPU=str(args.cpu))
        for key in ('YCSB_UB_SUITE','YCSB_UB_RPC','YCSB_UB_A_WRITABLE','YCSB_UB_SEED','YCSB_UB_SHERMAN_ACCESS'):
            env.pop(key,None)
        if getattr(args,'suite',False):
            env.update(YCSB_UB_SUITE='1',YCSB_UB_RPC=str(args.rpc),
                       YCSB_UB_A_WRITABLE=str(args.a_writable),YCSB_UB_SEED=str(args.seed),
                       YCSB_UB_SHERMAN_ACCESS=args.sherman_access)
        command=[args.binary, '-db', getattr(args,'db','bwtree'), '-machinenum', '2',
                 '-machineno', '0' if args.role=='B' else '1',
                 '-server_threads', str(args.threads_a+args.threads_b),
                 '-dbnum', '1', '-valuesize', str(getattr(args, 'value_size', 8)), '-P', args.spec, '-C', args.ini]
        host_threads=args.threads_a if args.role=='A' else args.threads_b
        cpus=select_cpus(host_threads,args.cpu,os.sched_getaffinity(0))
        env['YCSB_UB_CPUS']=','.join(map(str,cpus))
        command=['taskset','-c',env['YCSB_UB_CPUS']]+command
        print(f'[{args.role}] Worker threads={host_threads}; CPU IDs={cpus}',flush=True)
        # SDK output can interleave at byte boundaries. Never parse stdout for control.
        read_fd, write_fd = os.pipe()
        env['YCSB_UB_CONTROL_FD'] = str(write_fd)
        try:
            self.process = subprocess.Popen(command, env=env, pass_fds=(write_fd,),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1)
        except BaseException:
            os.close(read_fd)
            raise
        finally:
            os.close(write_fd)
        def read_control():
            try:
                with os.fdopen(read_fd, 'r') as control:
                    for line in control:
                        self.queue.put(line.rstrip('\n'))
            finally:
                self.queue.put('EOF')
        def read():
            with self.process.stdout:
                for line in self.process.stdout:
                    self.log_tail.append(line.rstrip()[-512:])
                    log.write(line)
                    log.flush()
                    if line.startswith(('UB_', 'YCSB_STAGE ')):
                        self.last_stage = line.strip()
                    print(f'[{args.role} worker] {line.rstrip()}', flush=True)
        self.protocol_reader = threading.Thread(target=read_control, daemon=True)
        self.protocol_reader.start()
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.timeout = args.timeout

    def receive(self):
        start = time.monotonic()
        deadline = start + self.timeout
        next_notice = start + 10
        while True:
            self.peer_check()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f'worker pid={self.process.pid} response timed out after {self.timeout}s; last stage: {self.last_stage}; inspect worker.log')
            try:
                response = self.queue.get(timeout=min(0.1, remaining))
                break
            except queue.Empty:
                if time.monotonic() >= next_notice:
                    print(f'[{self.role}] Waiting for worker response ({time.monotonic()-start:.0f}s); pid={self.process.pid}; last stage: {self.last_stage}', flush=True)
                    next_notice = time.monotonic()+10
        if response == 'EOF':
            self.raise_exit_error()
        if response.startswith('FAULT '):
            self.raise_fault(response)
        return response

    def raise_fault(self, response):
        fields=dict(part.split('=',1) for part in response.split()[1:])
        address=int(fields['address'],0)
        region='outside UB mappings'
        if 0x300000000000 <= address < 0x300000000000+self.data_size:
            region='CACHE data (A: '+('read-write' if getattr(self,'a_writable',False) else 'read-only')+'; B: read-write)'
        elif 0x400000000000 <= address < 0x400000000000+self.nc_size:
            region='NC metadata (both read-write)'
        location=''
        if shutil.which('addr2line'):
            try:
                symbol=subprocess.run(['addr2line','-C','-f','-i','-e',self.binary,fields['pc']],
                    capture_output=True,text=True,timeout=3)
                if symbol.returncode==0:
                    location='; executable symbolization: '+symbol.stdout.strip()
            except (OSError,subprocess.TimeoutExpired):
                pass
        raise RuntimeError(f'{self.role} worker {response}; region={region}{location}')

    def raise_exit_error(self):
        try:
            code = self.process.wait(timeout=0.2)
        except subprocess.TimeoutExpired:
            code = None
        self.reader.join(timeout=0.2)
        state = f'worker exited with code {code}' if code is not None else 'worker control pipe closed while process is still running'
        raise RuntimeError(f'{state}; pid={self.process.pid}; last stage: {self.last_stage}; '
                           'recent worker output:\n' + '\n'.join(list(self.log_tail)))

    def check_alive(self):
        if not self.finished and self.process.poll() is not None:
            self.raise_exit_error()

    def send(self, command):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def command(self, command, expected):
        self.send(command)
        response = self.receive()
        if response != expected:
            raise RuntimeError(command + ': ' + response)

    def finish(self, command):
        self.command(command, 'CLOSED')
        deadline = time.monotonic()+self.timeout
        while self.process.poll() is None:
            self.peer_check()
            if time.monotonic() >= deadline:
                raise TimeoutError('worker cleanup timed out')
            time.sleep(0.1)
        if self.process.returncode:
            self.raise_exit_error()
        self.finished = True
        self.process.stdin.close()
        self.protocol_reader.join(timeout=1)


def summary(a, b, seconds, ops, suite=False, rpc=0, a_writable=0):
    for role, result in [('A', a), ('B', b)]:
        if result['role'] != role or result['reads'] + result['writes'] != ops or result['errors']:
            raise RuntimeError('incomplete/failed workload on ' + role)
    if (a_writable or a['writes']) or seconds <= 0:
        raise RuntimeError('invalid read-only workload or duration')
    if b['framework_total_operations'] != 2*ops or a['bwtree_opt'] != b['bwtree_opt']:
        raise RuntimeError('framework result mismatch')
    control_seconds=seconds
    seconds=b['seconds']
    if seconds <= 0: raise RuntimeError('invalid framework duration')
    if suite:
        expected_rpc=ops if rpc else 0
        if b.get('rpc_completed')!=expected_rpc:
            raise RuntimeError('RPC completion count mismatch')
        transport='mpmc-shared-queue' if rpc else 'none'
        if any(r.get('rpc_transport')!=transport for r in (a,b)):
            raise RuntimeError('RPC transport mismatch; rebuild bundles')
    return {'framework': 'tests/YCSB-C/ycsbc', 'controller_seconds': control_seconds, 'pass': True, 'A': a, 'B': b, 'total_operations': 2*ops,
            'common_seconds': seconds, 'total_ops_per_second': 2*ops/seconds,
            'total_mops': 2*ops/seconds/1e6, 'update_semantics': 'replace-existing-key' if suite else 'fresh-insert',
            'A_observed_read_fraction': a['reads']/ops,
            'B_observed_read_fraction': b['reads']/ops,
            'cache_policy': 'A-RO/B-RW',
            'reclamation': 'retain-until-both-processes-exit', 'bwtree_opt': b.get('bwtree_opt', False)}


def check_thread_sweep(identity, peer):
    sweep = identity.get('thread_sweep')
    if sweep != peer.get('thread_sweep'):
        raise RuntimeError('configuration mismatch: thread sweep schedule/round')
    if sweep is not None:
        expected = json.loads(sweep)['threads']
        if identity['threads'] != expected or peer['threads'] != expected:
            raise RuntimeError('thread sweep requires both worker counts to match the requested round')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role', choices=['A', 'B'], required=True)
    p.add_argument('--peer', required=True)
    p.add_argument('--provider', default='')
    p.add_argument('--port', type=int, default=18982)
    p.add_argument('--binary', required=True)
    p.add_argument('--spec', required=True)
    p.add_argument('--ini', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--threads', type=int, default=1)
    p.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
    p.add_argument('--size-mb', type=int, default=32768)
    p.add_argument('--nc-size-mb', type=int, default=16384)
    p.add_argument('--records', type=int, default=10000000)
    p.add_argument('--ops', type=int, default=10000000)
    p.add_argument('--timeout', type=int, default=1200)
    p.add_argument('--suite', action='store_true')
    p.add_argument('--case', default='bwtree')
    p.add_argument('--db', choices=['bwtree','clevelhash','sherman'], default='bwtree')
    p.add_argument('--rpc', type=int, choices=[0,1], default=0)
    p.add_argument('--expected-opt', type=int, choices=[0,1], help='Reject a reused binary with the wrong OPT mode')
    p.add_argument('--sherman-access', choices=['cache','nc-direct','nc-memcpy'], default='cache')
    p.add_argument('--a-writable', type=int, choices=[0], default=0, help='A is always read-only')
    p.add_argument('--seed', type=int, default=20261104)
    p.add_argument('--fixture', action='store_true', help='Explicit local file-backed SDK correctness test only')
    args = p.parse_args()
    try:
        args.worker_cpus=select_cpus(args.threads,args.cpu,os.sched_getaffinity(0))
    except ValueError as error:
        p.error(str(error))
    if args.fixture and not os.environ.get('UBSM_TEST_DIRECTORY'):
        p.error('--fixture requires UBSM_TEST_DIRECTORY')
    worker = channel = connection = server = None
    report = {'pass': False}
    log = (args.output/'worker.log').open('w')
    control_log = (args.output/'control.log').open('w')
    stage = 'starting'
    def status(message):
        nonlocal stage
        stage = message
        line = f'[{args.role}] {message}'
        print(line, flush=True)
        control_log.write(line + '\n'); control_log.flush()
    def send(message):
        channel.send(message)
    def receive():
        return channel.receive(worker.check_alive if worker else lambda: None)
    try:
        if args.role == 'B':
            server = socket.socket()
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((args.peer, args.port)); server.listen(1); server.settimeout(args.timeout)
            status(f'Listening on {args.peer}:{args.port}; waiting for A')
            connection, remote_address = server.accept()
            status(f'TCP connected from {remote_address[0]}:{remote_address[1]}')
        else:
            status(f'Connecting to B at {args.peer}:{args.port}')
            deadline = time.monotonic()+args.timeout
            last_notice = 0.0
            while True:
                try:
                    connection = socket.create_connection((args.peer,args.port), timeout=3)
                    status(f'TCP connected to B at {args.peer}:{args.port}')
                    break
                except OSError as error:
                    if time.monotonic() >= deadline:
                        raise
                    if time.monotonic()-last_notice >= 10:
                        status(f'Connection not established: {error}; retrying {args.peer}:{args.port}')
                        last_notice = time.monotonic()
                    time.sleep(1)
        connection.settimeout(args.timeout)
        channel = PeerChannel(connection, args.timeout)
        identity = {'host':socket.gethostname(), 'role':args.role, 'provider':args.provider,
                    'binary_sha256':hashlib.sha256(Path(args.binary).read_bytes()).hexdigest(),
                    'spec_sha256':hashlib.sha256(Path(args.spec).read_bytes()).hexdigest(),
                    'threads':args.threads,'size_mb':args.size_mb,'nc_size_mb':args.nc_size_mb,'records':args.records,'ops':args.ops,
                    'case':args.case,'suite':args.suite,'db':args.db,'rpc':args.rpc,'expected_opt':args.expected_opt,
                    'sherman_access':args.sherman_access,'a_writable':args.a_writable,'seed':args.seed,'fixture':args.fixture,
                    'thread_sweep':os.environ.get('YCSB_UB_THREAD_SWEEP'),
                    'worker_cpus':args.worker_cpus}
        status('TCP connected; exchanging configuration and binary hashes')
        send(identity); peer = receive()
        if peer['role'] == args.role or (not args.fixture and peer['host'] == identity['host']):
            raise RuntimeError('expected two different physical hosts and roles')
        check_thread_sweep(identity, peer)
        for key in ('provider','binary_sha256','spec_sha256','size_mb','nc_size_mb','records','ops','case','suite','db','rpc','expected_opt','sherman_access','a_writable','seed','fixture'):
            if identity[key] != peer[key]:
                raise RuntimeError('configuration mismatch: '+key)
        args.threads_a=args.threads if args.role=='A' else peer['threads']
        args.threads_b=args.threads if args.role=='B' else peer['threads']
        provider = args.provider or (identity['host'] if args.role=='B' else peer['host'])
        status(f'Handshake complete; UB provider={provider}')
        if args.role=='B':
            name='ycsb-ub-'+uuid.uuid4().hex[:18]; send(name)
        else:
            name=receive()
            status(f'Waiting for B to initialize UB and load {args.db}; object={name}')
            if receive()!='LOADED': raise RuntimeError('B did not load')
        (args.output/'config.json').write_text(json.dumps({**vars(args),'name':name,'provider':provider,
                                                        'identity':identity,'peer':peer,
                                                        'workload':Path(args.spec).read_text()},default=str,indent=2))
        status('Starting host worker: UB mapping, initial load (B only), and trace generation')
        worker=Worker(args,name,provider,log)
        worker.peer_check = channel.check
        if worker.receive()!='READY': raise RuntimeError('expected READY')
        status('Host worker READY')
        if args.role=='B':
            status('B load complete; waiting for A to map and prepare')
            send('LOADED')
            if receive()!='RUN': raise RuntimeError('expected RUN')
            status('Running YCSB workload')
            worker.send('run')
            result=worker.receive()
            if not result.startswith('RESULT '): raise RuntimeError(result)
            send(json.loads(result[7:]))
            if receive()!='RELEASE': raise RuntimeError('expected RELEASE after A cleanup')
            worker.finish('release'); send('CLOSED')
            report=receive()
        else:
            status('Both workers READY; starting YCSB workload')
            # One monotonic clock covers both starts and both completions.
            start=time.monotonic()
            send('RUN'); worker.send('run')
            result=worker.receive()
            if not result.startswith('RESULT '): raise RuntimeError(result)
            a=json.loads(result[7:]); b=receive()
            elapsed=time.monotonic()-start
            if args.expected_opt is not None and any(r['bwtree_opt'] != bool(args.expected_opt) for r in (a,b)):
                raise RuntimeError('Binary OPT mode does not match requested case; rebuild bundles')
            if args.db=='sherman' and args.sherman_access!='cache':
                if any(r.get('nc_data') is not True or r.get('nc_direct') != (args.sherman_access=='nc-direct') for r in (a,b)):
                    raise RuntimeError('Worker does not implement the requested NC access mode; rebuild bundles')
            status('Original YCSB execution complete; collecting native throughput and cleaning up')
            report=summary(a,b,elapsed,args.ops,args.suite,args.rpc,args.a_writable)
            report.update(case=args.case,db=args.db,rpc=bool(args.rpc),seed=args.seed,fixture=args.fixture,
                          sherman_access=args.sherman_access if args.db=='sherman' else None)
            if args.db=='sherman' and args.sherman_access!='cache':
                report['cache_policy']='NC-only; A reads, B reads/updates'
            worker.finish('close')
            send('RELEASE')
            if receive()!='CLOSED': raise RuntimeError('B cleanup failed')
            send(report)
    except Exception as error:
        report.update({'pass':False,'error':str(error),'stage':stage,'object_may_remain':True})
        if isinstance(error, PeerFailure):
            report['peer_error'] = error.details
        elif channel is not None:
            # Tell the peer the original failure before closing TCP or waiting for cleanup.
            try:
                send({'type':'ERROR','role':args.role,'stage':stage,'error':str(error)[:16000]})
            except (OSError, ValueError):
                pass
        print(f'[{args.role}] ERROR at {stage}: {error}', flush=True)
        control_log.write(f'ERROR at {stage}: {error}\n'); control_log.flush()
    finally:
        if worker:
            if not report['pass'] and worker.process.poll() is None:
                worker.process.terminate()
                try:
                    worker.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    worker.process.kill()
                    try:
                        worker.process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        report['worker_exit_unconfirmed']=True
            worker.process.stdin.close()
            worker.reader.join(timeout=1)
            worker.protocol_reader.join(timeout=1)
        if channel: channel.close()
        for handle in (connection, server):
            if handle: handle.close()
        if not worker or not worker.reader.is_alive(): log.close()
        control_log.close()
        (args.output/'result.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2),flush=True)
    return 0 if report['pass'] else 1


if __name__=='__main__':
    raise SystemExit(main())
