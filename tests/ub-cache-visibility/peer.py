#!/usr/bin/env python3
"""One local worker per physical host; B owns memory, A coordinates over TCP."""
import argparse
import json
import os
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path
sys.dont_write_bytecode = True
from run import Worker, OLD, NEW


def check_nc(command, report):
    """Check returned values and both mappings after every sequential operation."""
    checks = report['checks'] = {}

    def expect(name, role, operation, expected):
        print(f'NC CHECK {name}: {role} {operation}', flush=True)
        item = checks[name] = {'role': role, 'command': operation,
                               'expected': expected, 'pass': False}
        response = item['actual'] = command(role, operation)
        item['pass'] = response == expected
        if not item['pass']:
            raise RuntimeError(f'{name}: expected {expected!r}, got {response!r}')
        return response

    def read_both(name, expected):
        values = {}
        for role in ('A', 'B'):
            response = expect(f'{name}_{role}_read', role, 'read', f'VALUE {expected}')
            values[role] = int(response.split()[1])
        return values

    report['initial'] = read_both('initial', OLD)['A']
    expect('B_write', 'B', 'write', 'OK')
    report['A_reads_B'] = read_both('B_write', NEW)['A']
    expect('A_write', 'A', 'write', 'OK')
    report['B_reads_A'] = read_both('A_write', OLD)['B']
    current = OLD
    for role, desired in (('A', NEW), ('B', OLD)):
        expect(f'{role}_cas_success', role, f'cas {current} {desired}', f'CAS 1 {current}')
        current = desired
        read_both(f'{role}_cas_success', current)
        # A failed CAS must return the observed value and leave memory unchanged.
        expect(f'{role}_cas_failure', role, f'cas {current ^ 1} {current ^ 2}',
               f'CAS 0 {current}')
        read_both(f'{role}_cas_failure', current)
    mask = (1 << 64) - 1
    for role, amount in (('A', (1 << 32) + 7), ('B', (1 << 33) + 9)):
        expect(f'{role}_fetch_add', role, f'fetch_add {amount}', f'FETCH_ADD {current}')
        current = (current + amount) & mask
        read_both(f'{role}_fetch_add', current)
    for role in ('A', 'B'):
        # Overflow from a high 64-bit value, then UINT64_MAX + 3 on the other host.
        amount = mask - current + 4 if role == 'A' else mask
        expect(f'{role}_fetch_add_wrap', role, f'fetch_add {amount}', f'FETCH_ADD {current}')
        current = (current + amount) & mask
        read_both(f'{role}_fetch_add_wrap', current)
    report['final'] = current


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
    nc = os.environ.get('UB_MICRO_NC') is not None
    args.timeout = int(os.environ.get('NC_TIMEOUT', '120')) if nc else 60
    args.cpu_a = args.cpu_b = args.cpu
    args.size_mb, args.base, args.offset = 128, '0x300000000000', '0'
    if nc:
        args.size_mb = int(os.environ.get('NC_SIZE_MB', '4'))
        args.base = '0x400000000000'
    mode = {'nc': nc, 'size_mb': args.size_mb, 'base': args.base,
            'map_mode': os.environ.get('NC_MAP_MODE', 'noreplace')}
    if nc:
        mode['nc_protocol'] = 2
    worker = connection = server = stream = None
    report = {'pass': False, **mode}
    log = (args.output / 'worker.log').open('w')
    def send(value):
        stream.write((json.dumps(value) + '\n').encode())
        stream.flush()

    def receive():
        line = stream.readline(65536)
        if not line:
            raise RuntimeError('peer disconnected; object may remain')
        message = json.loads(line)
        if isinstance(message, dict) and 'error' in message:
            raise RuntimeError('peer failed: ' + str(message['error']))
        return message

    def local(command, expected=None):
        response = worker.command(command, args)
        if expected and response != expected:
            raise RuntimeError('unexpected response: ' + response)
        return response

    def remote(command, expected='OK'):
        send(command)
        response = receive()
        if expected is not None and response != expected:
            raise RuntimeError('peer failed: ' + str(response))
        return response

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
        # Both builds have finished before UB initialization.
        send({'host': socket.gethostname(), 'provider': args.provider, **mode})
        peer = receive()
        if peer['host'] == socket.gethostname() or peer['provider'] != args.provider:
            raise RuntimeError('hosts must differ and provider must match')
        if any(peer.get(key) != value for key, value in mode.items()):
            raise RuntimeError('A/B NC configuration mismatch')
        args.provider = args.provider or (socket.gethostname() if args.role == 'B' else peer['host'])
        print('UB provider: ' + args.provider, flush=True)
        if args.role == 'B':
            args.name = 'ub-vis-' + uuid.uuid4().hex[:20]
            send(args.name)
        else:
            args.name = receive()
        report['name'] = args.name
        if args.role == 'A' and receive() != 'READY':
            raise RuntimeError('B did not initialize')
        worker = Worker(None, args.binary, args.role, args, log)
        if worker.receive(args) != 'READY':
            raise RuntimeError('worker did not initialize')
        if args.role == 'B':
            send('READY')
            if nc:
                while True:
                    operation = receive()
                    if operation == 'release':
                        local('release', 'CLOSED')
                        if worker.process.wait(timeout=args.timeout):
                            raise RuntimeError('B cleanup failed')
                        send('OK')
                        break
                    if (not isinstance(operation, str) or '\n' in operation or '\r' in operation
                            or not (operation in ('read', 'write')
                                    or operation.startswith(('cas ', 'fetch_add ')))):
                        raise RuntimeError('unexpected NC protocol command: ' + str(operation))
                    send(local(operation))
            else:
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
            if nc:
                check_nc(lambda role, operation: local(operation) if role == 'A'
                         else remote(operation, expected=None), report)
            else:
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
            report['pass'] = True if nc else report['case1_old'] and report['case2_new']
            send(report)
    except Exception as error:
        report.update(error=str(error), object_may_remain=True, **{'pass': False})
        if stream:
            try:
                send({'error': str(error)})
            except (OSError, ValueError):
                pass
    finally:
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
        # A blocked SDK call can leave the daemon log reader alive until process exit.
        if not worker or not worker.reader.is_alive():
            log.close()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
