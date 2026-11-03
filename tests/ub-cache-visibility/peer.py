#!/usr/bin/env python3
"""One local worker per physical host; B owns memory, A coordinates over TCP."""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
sys.dont_write_bytecode = True
from run import Worker, OLD, NEW


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role', choices=['A', 'B'], required=True)
    p.add_argument('--peer', required=True)
    p.add_argument('--port', type=int, required=True)
    p.add_argument('--provider', required=True)
    p.add_argument('--binary', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
    args = p.parse_args()
    args.timeout = 60
    args.cpu_a = args.cpu_b = args.cpu
    args.size_mb, args.base, args.offset = 128, '0x300000000000', '0'
    failed, stop = threading.Event(), threading.Event()
    worker = connection = server = stream = monitor = None
    report = {'pass': False, 'samples_valid': False}
    log = (args.output / 'worker.log').open('w')
    resource_log = (args.output / 'resources.log').open('w')

    def check_resources():
        # Conservative check of known experiment workers AND their launchers.
        # Log all processes so the operator can also audit site-specific jobs.
        snapshot = subprocess.check_output(
            ['ps', '-eo', 'pid=,ppid=,pcpu=,args='], text=True)
        resource_log.write(time.strftime('%Y-%m-%dT%H:%M:%S%z') + ' ' + socket.gethostname() + '\n' + snapshot)
        resource_log.flush()
        rows = [line.strip().split(None, 3) for line in snapshot.splitlines()]
        own = {os.getpid()}
        parents = {int(row[0]): int(row[1]) for row in rows if len(row) == 4}
        parent = os.getpid()
        while parent in parents and parent not in (0, 1):
            own.add(parent)
            parent = parents[parent]
        if worker:
            own.add(worker.process.pid)
        pattern = re.compile(r'ycsb|ub_sdk_smoke|run_ub_single|run_cache_audit|'
                             r'benchmark|sysbench|stress-ng|(^|[/ ])perf( |$)|'
                             r'(^|[/ ])bench( |$)|tests/.*/run[^ /]*\.(py|sh)', re.I)
        for row in rows:
            if len(row) != 4 or int(row[0]) in own:
                continue
            if pattern.search(row[3]) or float(row[2]) > 80:
                raise RuntimeError('Other workload/driver detected: ' + ' '.join(row))

    def send(value):
        stream.write((json.dumps(value) + '\n').encode())
        stream.flush()

    def receive():
        line = stream.readline(8192)
        if not line:
            raise RuntimeError('peer disconnected; object may remain')
        return json.loads(line)

    def local(command, expected=None):
        response = worker.command(command, args, failed)
        if expected and response != expected:
            raise RuntimeError('unexpected response: ' + response)
        return response

    def remote(command):
        send(command)
        response = receive()
        if response != 'OK':
            raise RuntimeError('peer failed: ' + str(response))

    try:
        print('Waiting for the other host (up to 10 minutes)...', flush=True)
        if args.role == 'B':
            server = socket.socket()
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((args.peer, args.port))
            server.listen(1)
            server.settimeout(600)
            connection, _ = server.accept()
        else:
            deadline = time.monotonic() + 600
            while True:
                try:
                    connection = socket.create_connection((args.peer, args.port), timeout=3)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(1)
        connection.settimeout(args.timeout)
        stream = connection.makefile('rwb')
        # Both builds have finished before resource checks or UB initialization.
        send({'host': socket.gethostname(), 'provider': args.provider})
        peer = receive()
        if peer['host'] == socket.gethostname() or peer['provider'] != args.provider:
            raise RuntimeError('hosts must differ and provider must match')
        args.provider = args.provider or (socket.gethostname() if args.role == 'B' else peer['host'])
        print('UB provider: ' + args.provider, flush=True)
        for _ in range(2):
            check_resources()
            time.sleep(1)
        send('EXCLUSIVE')
        if receive() != 'EXCLUSIVE':
            raise RuntimeError('peer exclusivity check failed')
        def watch():
            while not stop.wait(1):
                try:
                    check_resources()
                except Exception as error:
                    print(str(error), file=sys.stderr, flush=True)
                    failed.set()
                    # Interrupt blocking control IO and only our own worker.
                    if worker and worker.process.poll() is None:
                        worker.process.terminate()
                    try:
                        connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    return
        monitor = threading.Thread(target=watch, daemon=True)
        monitor.start()
        if args.role == 'B':
            args.name = 'ub-vis-' + uuid.uuid4().hex[:20]
            send(args.name)
        else:
            args.name = receive()
        report['name'] = args.name
        if args.role == 'A' and receive() != 'READY':
            raise RuntimeError('B did not initialize')
        worker = Worker(None, args.binary, args.role, args, log)
        if worker.receive(args, failed) != 'READY':
            raise RuntimeError('worker did not initialize')
        if args.role == 'B':
            send('READY')
            for expected in ('write', 'flush', 'release'):
                if receive() != expected:
                    raise RuntimeError('unexpected protocol command')
                local(expected, 'CLOSED' if expected == 'release' else 'OK')
                if expected == 'release' and worker.process.wait(timeout=args.timeout):
                    raise RuntimeError('B cleanup failed')
                send('OK')
            report = receive()
        else:
            def value(command):
                response = local(command)
                if not response.startswith('VALUE '):
                    raise RuntimeError('expected VALUE')
                return int(response.split()[1])
            report['initial'] = value('prime')
            if report['initial'] != OLD:
                raise RuntimeError('initial value is not OLD')
            remote('write')
            report['after_write'] = value('read')
            remote('flush')
            report['after_flush_invalidate'] = value('invalidate')
            report['case1_old'] = report['after_write'] == OLD
            report['case2_new'] = report['after_flush_invalidate'] == NEW
            local('close', 'CLOSED')
            if worker.process.wait(timeout=args.timeout):
                raise RuntimeError('A cleanup failed; retain object')
            remote('release')
            report['samples_valid'] = not failed.is_set()
            report['pass'] = report['samples_valid'] and report['case1_old'] and report['case2_new']
            send(report)
    except Exception as error:
        report.update(error=str(error), object_may_remain=True, **{'pass': False})
    finally:
        stop.set()
        if monitor:
            monitor.join(timeout=5)
        if worker:
            try:
                worker.process.stdin.close()
                worker.process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                worker.process.terminate()
                report['worker_exit_unconfirmed'] = True
            worker.reader.join(timeout=1)
        for handle in (stream, connection, server):
            if handle:
                handle.close()
        if failed.is_set():
            report['samples_valid'] = report['pass'] = False
        resource_log.close()
        # A blocked SDK call can leave the daemon log reader alive until process exit.
        if not worker or not worker.reader.is_alive():
            log.close()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
