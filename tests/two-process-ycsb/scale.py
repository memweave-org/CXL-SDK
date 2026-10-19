#!/usr/bin/env python3
"""Build selected indexes, run a two-process thread sweep, and plot measured results."""
import argparse
import csv
import json
from pathlib import Path
import statistics
import subprocess
import sys

from run import CASES, ROOT

LABELS = {'sherman': 'Sherman', 'rpc': 'RPC (BwTree)',
          'bwtree-on': 'BwTree OPT', 'bwtree-off': 'BwTree NOOPT', 'bwtree-cc': 'BwTree CC',
          'clevelhash-on': 'ClevelHash OPT', 'clevelhash-off': 'ClevelHash NOOPT',
          'clevelhash-cc': 'ClevelHash CC'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    p.add_argument('--threads', nargs='+', type=int, default=list(range(24, 193, 24)))
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--records', type=int, default=100000)
    p.add_argument('--ops', type=int, default=200000)
    p.add_argument('--size-mb', type=int, default=1024)
    p.add_argument('--mapping-bits', type=int, default=16)
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--skip-build', action='store_true')
    p.add_argument('--host', help='second machine via SSH (same paths and binaries)')
    p.add_argument('--memory', choices=['file', 'cxl', 'ub'], default='file')
    p.add_argument('--shared-path')
    p.add_argument('--ub-library')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.repeats < 1 or any(t < 1 or t > 192 for t in a.threads):
        p.error('repeats >= 1 and threads 1..192 required')
    # Fail before starting the benchmark if plotting dependencies are absent.
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    if a.host and a.memory == 'file':
        p.error('two hosts require truly shared CXL/UB memory')
    if a.memory != 'file' and not a.shared_path:
        p.error('--shared-path is required for CXL/UB')
    if a.memory == 'ub' and not a.ub_library:
        p.error('--memory ub requires --ub-library')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'config.json').write_text(json.dumps(vars(a), default=str, indent=2) + '\n')
    rows = []
    built = a.skip_build
    for threads in sorted(set(a.threads)):
        for repeat in range(1, a.repeats + 1):
            run_out = out / f't{threads}-r{repeat}'
            cmd = [sys.executable, str(ROOT / 'tests/two-process-ycsb/run.py'),
                   '--cases', *a.cases, '--threads', str(threads), '--updates', 'fresh-insert',
                   '--output', str(run_out)]
            for key in ('records', 'ops', 'size_mb', 'mapping_bits', 'jobs', 'timeout'):
                cmd += ['--' + key.replace('_', '-'), str(getattr(a, key))]
            for key in ('host', 'memory', 'shared_path', 'ub_library'):
                if getattr(a, key):
                    cmd += ['--' + key.replace('_', '-'), str(getattr(a, key))]
            if built:
                cmd.append('--skip-build')
            print(f'Threads per process={threads}, repeat={repeat}', flush=True)
            subprocess.run(cmd, check=True)
            built = True
            for event in json.loads((run_out / 'results.json').read_text()):
                if event['status'] != 'passed' or event['memlink_test_stub']:
                    raise RuntimeError('Only validated real-process results may be plotted')
                results = event['results']
                # RPC counts only client work/time; direct modes use both workers
                # and the longer measured duration of the synchronized run.
                measured = results[1:] if event['case'] == 'rpc' else results
                seconds = max(float(r['seconds']) for r in measured)
                completed = sum(int(r['completed']) for r in measured)
                rows.append(dict(case=event['case'], threads_per_process=threads,
                                 total_threads=2*threads, repeat=repeat, completed=completed,
                                 seconds=seconds, mops=completed/seconds/1e6))
            with (out / 'measurements.csv').open('w') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader(); writer.writerows(rows)
    fig, ax = plt.subplots(figsize=(11, 6.5))
    summary = []
    for case in a.cases:
        points = []
        for t in sorted(set(a.threads)):
            values = [r['mops'] for r in rows if r['case'] == case and r['threads_per_process'] == t]
            median = statistics.median(values)
            points.append((t, median, min(values), max(values)))
            summary.append(dict(case=case, threads_per_process=t, median_mops=median,
                                min_mops=min(values), max_mops=max(values)))
        x, y, low, high = zip(*points)
        line, = ax.plot(x, y, marker='o', label=LABELS[case])
        if a.repeats > 1:
            ax.fill_between(x, low, high, color=line.get_color(), alpha=.12)
    environment = 'two hosts' if a.host else 'local two-process simulation'
    ax.set(xlabel='Threads per process (two processes; RPC: N servers + N clients)',
           ylabel='Measured throughput (million ops/s)',
           title='Two-process shared-index YCSB-B-derived workload\n95% reads / 5% fresh inserts, Zipfian; '
                 f'{environment}, backend={a.memory}')
    ax.set_xticks(sorted(set(a.threads)))
    ax.grid(alpha=.25); ax.legend(ncol=3, fontsize=9)
    fig.text(.5, .015, f'{a.records:,} initial keys; {a.ops:,} ops/process (RPC: client only); '
             f'{a.repeats} repeats, median and min/max; process-lifetime reclamation', ha='center', fontsize=9)
    fig.tight_layout(rect=(0, .045, 1, 1))
    for suffix in ('png', 'svg'):
        fig.savefig(out / f'ycsb-thread-scaling.{suffix}', dpi=180)
    with (out / 'summary.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(summary[0]))
        writer.writeheader(); writer.writerows(summary)
    print(f'Results and plot: {out}', flush=True)


if __name__ == '__main__':
    main()
