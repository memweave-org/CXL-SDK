"""Best-effort host-wide test exclusion with persistent process/CPU evidence."""
import json
import os
from pathlib import Path
import re
import socket
import threading
import time


class ResourceGuard:
    pattern = re.compile(r'bench|ycsbc|/(?:experiments|benchmark_results)/|(?:^|/)(?:direct|stress(?:-ng)?|fio|iperf\d*|perf|pytest|ctest)$|'
                         r'(?:run|test|verify|sweep|scale)[^/]*\.(?:py|sh)$', re.I)

    def __init__(self, output):
        self.log = (output/'exclusivity.jsonl').open('w')
        self.stop = threading.Event()
        self.conflict = threading.Event()
        self.previous = {}
        self.tracked = set()  # Keep observed drivers until the whole round exits.
        self.thread = None

    def snapshot(self, phase):
        now = time.monotonic()
        processes = {}
        for path in Path('/proc').iterdir():
            if not path.name.isdigit():
                continue
            try:
                argv = [x.decode(errors='replace') for x in (path/'cmdline').read_bytes().split(b'\0') if x]
                if not argv:
                    continue
                stat = (path/'stat').read_text().rsplit(')', 1)[1].split()
                pid, ticks, start = int(path.name), int(stat[11])+int(stat[12]), int(stat[19])
                old = self.previous.get((pid, start))
                cpu = None if old is None else 100*(ticks-old[0])/os.sysconf('SC_CLK_TCK')/max(now-old[1], .001)
                processes[pid] = dict(pid=pid, ppid=int(stat[1]), state=stat[0], argv=argv,
                                      start_ticks=start, cpu_ticks=ticks, cpu_percent=cpu)
            except (OSError, ValueError, IndexError):
                continue
        ancestors, own = set(), {os.getpid()}
        pid = os.getppid()
        while pid in processes and pid not in ancestors:
            ancestors.add(pid)
            pid = processes[pid]['ppid']
        while True:
            children = {pid for pid, p in processes.items() if p['ppid'] in own}
            if children <= own:
                break
            own |= children
        candidates = set()
        wrappers = ('timeout', 'numactl', 'taskset', 'time', 'env', 'stdbuf', 'perf')
        for pid, p in processes.items():
            if pid in own or pid in ancestors:
                continue
            argv = p['argv']
            exe = Path(argv[0]).name
            inspected = argv[:1]
            if exe.startswith(('python', 'bash', 'sh', 'zsh')):
                # Inspect the executed script, not filenames passed as data to it
                # (e.g. the system crash reporter's ycsbc filename).
                inspected = argv[:1]+next(([arg] for arg in argv[1:] if not arg.startswith('-')), [])
            elif exe in wrappers:
                inspected = [arg for arg in argv if not arg.startswith('-') and '\n' not in arg and ' ' not in arg]
            if any(self.pattern.search(arg) for arg in inspected):
                candidates.add(pid)
                # A driver with an arbitrary filename may launch many workers
                # separated by idle gaps. Track it, not just the active worker.
                parent = p['ppid']
                while parent in processes and parent not in own and parent not in ancestors:
                    driver = processes[parent]
                    executable = Path(driver['argv'][0]).name
                    if executable.startswith(('python', 'bash', 'sh', 'zsh')) or executable in wrappers:
                        candidates.add(parent)
                    else:
                        break
                    parent = driver['ppid']
        self.tracked.update((pid, processes[pid]['start_ticks']) for pid in candidates)
        self.tracked = {(pid, start) for pid, start in self.tracked
                        if pid in processes and processes[pid]['start_ticks'] == start}
        foreign = [processes[pid] for pid, _ in sorted(self.tracked)]
        exited = [pid for (pid, start) in self.previous if pid not in processes or processes[pid]['start_ticks'] != start]
        self.log.write(json.dumps(dict(time_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            host=socket.gethostname(), phase=phase, foreign_tests=foreign, exited_since_previous=exited,
            loadavg=Path('/proc/loadavg').read_text().strip(),
            system_cpu=Path('/proc/stat').read_text().splitlines()[0],
            top_cpu=sorted(processes.values(), key=lambda p: p['cpu_percent'] or 0, reverse=True)[:8],
            own_processes=[p for pid, p in processes.items() if pid in own]))+'\n')
        self.log.flush()
        self.previous = {(pid, p['start_ticks']): (p['cpu_ticks'], now) for pid, p in processes.items()}
        return foreign

    def start(self):
        clean = 0
        while clean < 3:
            foreign = self.snapshot('preflight')
            clean = 0 if foreign else clean+1
            if foreign:
                print('Waiting for other test drivers/workers to exit: '+str([p['pid'] for p in foreign]), flush=True)
            time.sleep(1 if not foreign else 5)
        self.thread = threading.Thread(target=self.monitor, daemon=True)
        self.thread.start()

    def monitor(self):
        while not self.stop.wait(.2):
            try:
                if self.snapshot('running'):
                    self.conflict.set()
            except Exception:
                self.conflict.set()  # Lost monitoring is not a successful exclusion check.

    def check(self):
        if self.conflict.is_set():
            raise RuntimeError('Another test appeared or monitoring failed; stop our workers and mark sample interfered')

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=3)
        if self.snapshot('finished'):
            self.conflict.set()
        self.log.close()
