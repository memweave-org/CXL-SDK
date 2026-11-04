#!/usr/bin/env python3
"""Start one local BwTree process; B owns the shared UB object, A coordinates."""
import argparse
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


class Worker:
    def __init__(self, args, name, provider, log):
        self.queue = queue.Queue()
        self.role = args.role
        self.process = subprocess.Popen([args.binary, args.role, name, provider,
            str(args.size_mb), str(args.threads), str(args.records), str(args.ops),
            args.spec, str(args.cpu)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        def read():
            for line in self.process.stdout:
                log.write(line)
                log.flush()
                if line.startswith('UBYCSB '):
                    self.queue.put(line.strip()[7:])
                else:
                    print(f'[{args.role} worker] {line.rstrip()}', flush=True)
            self.queue.put('EOF')
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.timeout = args.timeout

    def receive(self):
        start = time.monotonic()
        deadline = start + self.timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f'worker response timed out after {self.timeout}s; inspect worker.log')
            try:
                response = self.queue.get(timeout=min(10, remaining))
                break
            except queue.Empty:
                print(f'[{self.role}] Waiting for worker response ({time.monotonic()-start:.0f}s); see worker.log', flush=True)
        if response == 'EOF':
            raise RuntimeError('worker exited; inspect worker.log')
        return response

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
        if self.process.wait(timeout=self.timeout):
            raise RuntimeError('worker cleanup failed')


def summary(a, b, seconds, ops):
    for role, result in [('A', a), ('B', b)]:
        if result['role'] != role or result['reads'] + result['writes'] != ops or result['errors']:
            raise RuntimeError('incomplete/failed workload on ' + role)
    if a['writes'] or seconds <= 0:
        raise RuntimeError('invalid read-only workload or duration')
    return {'pass': True, 'A': a, 'B': b, 'total_operations': 2*ops,
            'common_seconds': seconds, 'total_ops_per_second': 2*ops/seconds,
            'total_mops': 2*ops/seconds/1e6, 'update_semantics': 'fresh-insert',
            'B_observed_read_fraction': b['reads']/ops,
            'cache_policy': 'writer-writeback-reader-invalidate',
            'reclamation': 'retain-until-both-processes-exit', 'bwtree_opt': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role', choices=['A', 'B'], required=True)
    p.add_argument('--peer', required=True)
    p.add_argument('--provider', default='')
    p.add_argument('--port', type=int, default=18982)
    p.add_argument('--binary', required=True)
    p.add_argument('--spec', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--threads', type=int, default=1)
    p.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
    p.add_argument('--size-mb', type=int, default=4096)
    p.add_argument('--records', type=int, default=100000)
    p.add_argument('--ops', type=int, default=1000000)
    p.add_argument('--timeout', type=int, default=1200)
    args = p.parse_args()
    worker = stream = connection = server = None
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
        stream.write((json.dumps(message)+'\n').encode()); stream.flush()
    def receive():
        data = stream.readline(65536)
        if not data:
            raise RuntimeError('peer disconnected')
        return json.loads(data)
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
        stream = connection.makefile('rwb')
        identity = {'host':socket.gethostname(), 'role':args.role, 'provider':args.provider,
                    'binary_sha256':hashlib.sha256(Path(args.binary).read_bytes()).hexdigest(),
                    'spec_sha256':hashlib.sha256(Path(args.spec).read_bytes()).hexdigest(),
                    'size_mb':args.size_mb,'records':args.records,'ops':args.ops}
        status('TCP connected; exchanging configuration and binary hashes')
        send(identity); peer = receive()
        if peer['role'] == args.role or peer['host'] == identity['host']:
            raise RuntimeError('expected two different physical hosts and roles')
        for key in ('provider','binary_sha256','spec_sha256','size_mb','records','ops'):
            if identity[key] != peer[key]:
                raise RuntimeError('configuration mismatch: '+key)
        provider = args.provider or (identity['host'] if args.role=='B' else peer['host'])
        status(f'Handshake complete; UB provider={provider}')
        if args.role=='B':
            name='ub-bwtree-'+uuid.uuid4().hex[:18]; send(name)
        else:
            name=receive()
            status(f'Waiting for B to initialize UB and load BwTree; object={name}')
            if receive()!='LOADED': raise RuntimeError('B did not load')
        (args.output/'config.json').write_text(json.dumps({**vars(args),'name':name,'provider':provider,
                                                        'identity':identity,'peer':peer,
                                                        'workload':Path(args.spec).read_text()},default=str,indent=2))
        status('Starting host worker: UB mapping, initial load (B only), and trace generation')
        worker=Worker(args,name,provider,log)
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
            status('Both workloads complete; verifying B inserts and cleaning up')
            report=summary(a,b,elapsed,args.ops)
            # Untimed: prove A can see B's inserted keys and representative initial keys.
            witnesses=[1,args.records,*b['witnesses']]
            worker.command('verify '+' '.join(map(str,witnesses)), 'VERIFIED')
            report['A_verified_B_witnesses']=sum(key!=0 for key in b['witnesses'])
            worker.finish('close')
            send('RELEASE')
            if receive()!='CLOSED': raise RuntimeError('B cleanup failed')
            send(report)
    except Exception as error:
        report.update({'pass':False,'error':str(error),'stage':stage,'object_may_remain':True})
    finally:
        if worker:
            try:
                worker.process.stdin.close(); worker.process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                worker.process.terminate(); report['worker_exit_unconfirmed']=True
            worker.reader.join(timeout=1)
        for handle in (stream, connection, server):
            if handle: handle.close()
        if not worker or not worker.reader.is_alive(): log.close()
        control_log.close()
        (args.output/'result.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2),flush=True)
    return 0 if report['pass'] else 1


if __name__=='__main__':
    raise SystemExit(main())
