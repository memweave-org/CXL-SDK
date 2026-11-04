#!/usr/bin/env python3
"""Launch the original ycsbc executable on each host; B is its master, A its follower."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import threading
import time
import uuid


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
        self.last_stage = 'process launch'
        self.peer_check = lambda: None
        self.finished = False
        self.log_tail = deque(maxlen=12)
        env=dict(os.environ, YCSB_UB_NAME=name, YCSB_UB_PROVIDER=provider,
                 YCSB_UB_THREADS_A=str(args.threads_a), YCSB_UB_THREADS_B=str(args.threads_b),
                 YCSB_UB_SIZE_MB=str(args.size_mb), YCSB_UB_NC_SIZE_MB=str(args.nc_size_mb),
                 YCSB_UB_RECORDS=str(args.records), YCSB_UB_OPS=str(args.ops), YCSB_UB_CPU=str(args.cpu))
        command=[args.binary, '-db', 'bwtree', '-machinenum', '2',
                 '-machineno', '0' if args.role=='B' else '1',
                 '-server_threads', str(args.threads_a+args.threads_b),
                 '-dbnum', '1', '-valuesize', '8', '-P', args.spec, '-C', args.ini]
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
        return response

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


def summary(a, b, seconds, ops):
    for role, result in [('A', a), ('B', b)]:
        if result['role'] != role or result['reads'] + result['writes'] != ops or result['errors']:
            raise RuntimeError('incomplete/failed workload on ' + role)
    if a['writes'] or seconds <= 0:
        raise RuntimeError('invalid read-only workload or duration')
    if b['framework_total_operations'] != 2*ops or a['bwtree_opt'] != b['bwtree_opt']:
        raise RuntimeError('framework result mismatch')
    control_seconds=seconds
    seconds=b['seconds']
    if seconds <= 0: raise RuntimeError('invalid framework duration')
    return {'framework': 'tests/YCSB-C/ycsbc', 'controller_seconds': control_seconds, 'pass': True, 'A': a, 'B': b, 'total_operations': 2*ops,
            'common_seconds': seconds, 'total_ops_per_second': 2*ops/seconds,
            'total_mops': 2*ops/seconds/1e6, 'update_semantics': 'fresh-insert',
            'B_observed_read_fraction': b['reads']/ops,
            'cache_policy': 'original-bwtree-split-ub',
            'reclamation': 'retain-until-both-processes-exit', 'bwtree_opt': b.get('bwtree_opt', False)}


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
    p.add_argument('--size-mb', type=int, default=4096)
    p.add_argument('--nc-size-mb', type=int, default=512)
    p.add_argument('--records', type=int, default=100000)
    p.add_argument('--ops', type=int, default=1000000)
    p.add_argument('--timeout', type=int, default=1200)
    args = p.parse_args()
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
                    'threads':args.threads,'size_mb':args.size_mb,'nc_size_mb':args.nc_size_mb,'records':args.records,'ops':args.ops}
        status('TCP connected; exchanging configuration and binary hashes')
        send(identity); peer = receive()
        if peer['role'] == args.role or peer['host'] == identity['host']:
            raise RuntimeError('expected two different physical hosts and roles')
        for key in ('provider','binary_sha256','spec_sha256','size_mb','nc_size_mb','records','ops'):
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
            status(f'Waiting for B to initialize UB and load BwTree; object={name}')
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
            status('Original YCSB execution complete; collecting native throughput and cleaning up')
            report=summary(a,b,elapsed,args.ops)
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
