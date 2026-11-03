#!/usr/bin/env python3
"""Drive two already-built SDK workers over SSH; no UB control flags in tested memory."""
import argparse
import json
import queue
import shlex
import subprocess
import threading
import time
import uuid
from pathlib import Path

OLD = 0x1122334455667788
NEW = 0x8877665544332211


class Worker:
    def __init__(self, host, binary, role, args, log):
        command = [binary, role, args.name, args.provider, str(args.size_mb),
                   args.base, args.offset, str(args.cpu_a if role == 'A' else args.cpu_b)]
        self.log = log
        self.messages = queue.Queue()
        launch = command if host is None else [
            'ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', host, shlex.join(command)]
        self.process = subprocess.Popen(
            launch, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        def read():
            for line in self.process.stdout:
                log.write(line)
                log.flush()
                if line.startswith('UBMICRO '):
                    self.messages.put(line.strip()[8:])
            self.messages.put('EOF')
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def receive(self, args, failed):
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if failed.is_set():
                raise RuntimeError('exclusivity guard failed; samples invalid')
            try:
                message = self.messages.get(timeout=0.1)
            except queue.Empty:
                continue
            if message == 'EOF':
                raise RuntimeError('worker exited unexpectedly; inspect logs')
            return message
        raise TimeoutError('worker timed out; inspect logs and remote processes')

    def command(self, command, args, failed):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()
        return self.receive(args, failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-a', required=True, help='SSH destination for read-only importer')
    parser.add_argument('--host-b', required=True, help='SSH destination for memory owner')
    parser.add_argument('--provider', required=True, help='B UB topology hostname')
    parser.add_argument('--binary-a', required=True, help='absolute remote worker path')
    parser.add_argument('--binary-b', required=True, help='absolute remote worker path')
    parser.add_argument('--cpu-a', type=int, required=True)
    parser.add_argument('--cpu-b', type=int, required=True)
    parser.add_argument('--size-mb', type=int, default=128)
    parser.add_argument('--base', default='0x300000000000')
    parser.add_argument('--offset', default='0')
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--guard', required=True, help='local executable checking BOTH hosts; see README')
    args = parser.parse_args()
    if args.host_a == args.host_b:
        parser.error('A and B must be distinct physical machines')
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    args.output.mkdir(parents=True, exist_ok=False)
    args.name = 'ub-vis-' + uuid.uuid4().hex[:20]
    (args.output / 'config.json').write_text(json.dumps(vars(args), default=str, indent=2))
    failed, stop = threading.Event(), threading.Event()
    guard_log = (args.output / 'exclusivity.log').open('w')

    def guard():
        guard_log.write(time.strftime('%Y-%m-%dT%H:%M:%S%z') + '\n')
        guard_log.flush()
        try:
            rc = subprocess.run([args.guard, args.host_a, args.host_b], stdout=guard_log,
                                stderr=subprocess.STDOUT, timeout=args.timeout).returncode
            if rc:
                failed.set()
        except Exception as error:
            guard_log.write(str(error) + '\n')
            failed.set()
        guard_log.flush()
        return not failed.is_set()

    workers, logs = [], []
    report = {'name': args.name, 'pass': False, 'samples_valid': False}
    monitor = None
    try:
        # Two successive whole-job checks, not an instantaneous utilization check.
        if not guard():
            raise RuntimeError('preflight exclusivity check failed')
        time.sleep(1)
        if not guard():
            raise RuntimeError('second preflight exclusivity check failed')
        def monitor_guard():
            while not stop.wait(1):
                if not guard():
                    return
        monitor = threading.Thread(target=monitor_guard, daemon=True)
        monitor.start()
        def start(role, host, binary):
            log = (args.output / (role + '.log')).open('w')
            logs.append(log)
            worker = Worker(host, binary, role, args, log)
            workers.append(worker)
            if worker.receive(args, failed) != 'READY':
                raise RuntimeError('expected READY')
            return worker
        b = start('B', args.host_b, args.binary_b)
        a = start('A', args.host_a, args.binary_a)
        def value(command):
            message = a.command(command, args, failed)
            if not message.startswith('VALUE '):
                raise RuntimeError('expected VALUE: ' + message)
            return int(message.split()[1])
        def expect(worker, command, expected):
            if worker.command(command, args, failed) != expected:
                raise RuntimeError('unexpected response to ' + command)
        report['initial'] = value('prime')
        if report['initial'] != OLD:
            raise RuntimeError('A did not read initial old value')
        expect(b, 'write', 'OK')
        report['after_write'] = value('read')
        expect(b, 'flush', 'OK')
        report['after_flush_invalidate'] = value('invalidate')
        report['case1_old'] = report['after_write'] == OLD
        report['case2_new'] = report['after_flush_invalidate'] == NEW
        expect(a, 'close', 'CLOSED')
        if a.process.wait(timeout=args.timeout):
            raise RuntimeError('A cleanup failed; retain object')
        expect(b, 'release', 'CLOSED')
        if b.process.wait(timeout=args.timeout):
            raise RuntimeError('B cleanup failed')
        report['samples_valid'] = not failed.is_set()
        report['pass'] = report['samples_valid'] and report['case1_old'] and report['case2_new']
    except Exception as error:
        report['error'] = str(error)
        report['object_may_remain'] = True
    finally:
        # EOF lets responsive workers unmap without deallocating. Never kill unrelated work.
        for worker in workers:
            try:
                worker.process.stdin.close()
                worker.process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                worker.process.terminate()
                report['remote_exit_unconfirmed'] = True
        stop.set()
        if monitor:
            monitor.join(timeout=args.timeout + 1)
        if failed.is_set():
            report['samples_valid'] = report['pass'] = False
        guard_log.close()
        for worker in workers:
            worker.reader.join(timeout=1)
        for log in logs:
            log.close()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
