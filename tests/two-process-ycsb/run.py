#!/usr/bin/env python3
"""Run two real worker processes against one shared index (local or SSH)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shlex
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
CASES = ['sherman', 'rpc', 'bwtree-off', 'bwtree-on', 'clevelhash-off', 'clevelhash-on', 'bwtree-cc', 'clevelhash-cc']

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    p.add_argument('--threads', type=int, nargs='+', default=[2], help='one common count or N0 N1; RPC: server/client counts')
    p.add_argument('--records', type=int, default=1000000)
    p.add_argument('--ops', type=int, default=1000000, help='operations per process (RPC: client process only)')
    p.add_argument('--size-mb', type=int, default=4096)
    p.add_argument('--timeout', type=int, default=1200)
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--mapping-bits', type=int, default=24)
    p.add_argument('--host', help='second host via SSH; identical repo/build absolute paths required')
    p.add_argument('--memory', choices=['file', 'cxl', 'ub'], default='ub' if os.getenv('SHM_UB_API', 'none') != 'none' else 'file')
    p.add_argument('--shared-path', help='CXL device / existing Memlink object / UBSM unique-name prefix')
    p.add_argument('--ub-api', choices=['none', 'memlink', 'ubsm'], default=os.getenv('SHM_UB_API'))
    p.add_argument('--ub-library', help='matching vendor library for the selected API')
    p.add_argument('--ub-include', default=os.getenv('UBSM_INCLUDE_DIR'), help='directory containing latest UBSM headers')
    p.add_argument('--provider-host', default=os.getenv('UBSM_PROVIDER_HOST'), help='owner hostname from UBS Engine topology')
    p.add_argument('--provider-numa', type=int, default=int(os.getenv('UBSM_PROVIDER_NUMA', '4294967295')))
    p.add_argument('--build-only', action='store_true')
    p.add_argument('--worker-prefix', default='', help='same local/remote worker launcher, e.g. docker exec ub-ycsb')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--skip-build', action='store_true')
    p.add_argument('--updates', choices=['original','fresh-insert'], default='original', help='original: BwTree/RPC fresh insert, other systems update existing keys')
    a = p.parse_args()
    a.ub_api = a.ub_api or ('memlink' if a.memory == 'ub' else 'none')
    if a.ub_api not in ('none', 'memlink', 'ubsm'):
        p.error('SHM_UB_API must be none, memlink or ubsm')
    if (a.memory == 'ub') != (a.ub_api != 'none'):
        p.error('--memory ub requires --ub-api memlink|ubsm; file/cxl require --ub-api none')
    a.ub_library = a.ub_library or os.getenv('UBSM_LIBRARY' if a.ub_api == 'ubsm' else 'UB_MEMLINK_LIBRARY')
    prefix = shlex.split(a.worker_prefix)
    if a.timeout < 1 or not 0 <= a.provider_numa <= 4294967295:
        p.error('timeout must be positive and provider-numa must fit uint32')
    if len(a.threads) not in (1, 2):
        p.error('--threads takes one or two counts')
    if len(a.threads) == 1:
        a.threads *= 2
    if any(not 1 <= t <= 192 for t in a.threads) or a.records < 2 or a.ops < 1 or a.size_mb < 128:
        p.error('threads 1..192, records >=2, ops >=1, size-mb >=128 required')
    if a.host and a.memory == 'file':
        p.error('two hosts require truly shared CXL/UB memory; an ordinary/NFS file is not a shared coherent arena')
    if not a.build_only and a.memory != 'file' and not a.shared_path:
        p.error('--shared-path is required for CXL/UB')
    if a.memory == 'ub' and not a.ub_library:
        p.error('--memory ub requires --ub-library; fallback is forbidden')
    if not a.build_only and a.ub_api == 'ubsm':
        if not a.provider_host:
            p.error('UBSM requires --provider-host (the physical owner in the UB topology)')
        if a.size_mb % 4:
            p.error('UBSM size-mb must be a multiple of 4')
        if not a.shared_path.isascii() or len(a.shared_path) > 35 or not all(c.isalnum() or c in '-_' for c in a.shared_path):
            p.error('UBSM shared-path is a unique-name prefix: 1..35 ASCII letters/digits/-/_')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = ROOT / 'tests/YCSB-C/workloads/workloadb_zipfian.spec'
    results = []
    for case in a.cases:
        system, _, opt = case.partition('-')
        build = ROOT / 'build/two-ycsb' / (case + ('-' + a.ub_api if a.memory == 'ub' else ''))
        exe = build / 'bench'
        if not a.skip_build:
            configure = ['cmake', '-S', str(ROOT/'tests/two-process-ycsb'), '-B', str(build),
                         f'-DSYSTEM={system}', f'-DOPT={"OFF" if opt in ("off", "cc") else "ON"}',
                         f'-DCC={"ON" if opt == "cc" else "OFF"}',
                         f'-DBWTREE_MAPPING_TABLE_BITS={a.mapping_bits}',
                         '-DWITH_UB=OFF', f'-DUB_API={a.ub_api}']
            if a.ub_library:
                configure += [f'-D{"UBSM_LIBRARY" if a.ub_api == "ubsm" else "UB_MEMLINK_LIBRARY"}={Path(a.ub_library).resolve()}']
            if a.ub_api == 'ubsm' and a.ub_include:
                configure += [f'-DUBSM_INCLUDE_DIR={Path(a.ub_include).resolve()}']
            with (out/f'{case}-build.log').open('w') as log:
                subprocess.run(configure, check=True, stdout=log, stderr=subprocess.STDOUT)
                subprocess.run(['cmake', '--build', str(build), '-j', str(a.jobs)], check=True, stdout=log, stderr=subprocess.STDOUT)
        if a.skip_build:
            cache = {}
            for line in (build/'CMakeCache.txt').read_text().splitlines():
                if '=' in line and ':' in line and not line.startswith(('//', '#')):
                    key, value = line.split('=', 1)
                    cache[key.split(':', 1)[0]] = value
            required = {'SYSTEM': system, 'OPT': 'OFF' if opt in ('off', 'cc') else 'ON',
                        'CC': 'ON' if opt == 'cc' else 'OFF',
                        'BWTREE_MAPPING_TABLE_BITS': str(a.mapping_bits),
                        'WITH_UB': 'OFF', 'UB_API': a.ub_api}
            if a.memory == 'ub':
                required['UBSM_LIBRARY' if a.ub_api == 'ubsm' else 'UB_MEMLINK_LIBRARY'] = str(Path(a.ub_library).resolve())
            if a.ub_api == 'ubsm' and a.ub_include:
                required['UBSM_INCLUDE_DIR'] = str(Path(a.ub_include).resolve())
            if any(cache.get(k) != v for k, v in required.items()):
                raise RuntimeError('--skip-build configuration differs from the requested flags/library')
        if a.build_only:
            print(f'{case}: BUILT api={a.ub_api}', flush=True)
            continue
        digest = hashlib.sha256(exe.read_bytes()).hexdigest()
        if a.host:
            remote = subprocess.check_output(['ssh', '-oBatchMode=yes', a.host,
                shlex.join(['sha256sum', str(exe), str(spec)])], text=True).splitlines()
            if len(remote) != 2 or remote[0].split()[0] != digest or remote[1].split()[0] != hashlib.sha256(spec.read_bytes()).hexdigest():
                raise RuntimeError('worker executable/workload differs between hosts; copy identical inputs first')
        backing = out/f'{case}.arena'
        path = a.shared_path
        if a.ub_api == 'ubsm':
            path = f'{a.shared_path}-{secrets.token_hex(6)}'
        if a.memory == 'file':
            with backing.open('xb') as f:
                f.truncate(a.size_mb << 20)
            path = str(backing)
        token = secrets.randbits(63) or 1
        commands = []
        for i in range(2):
            cmd = [str(exe), str(i), ','.join(map(str,a.threads)), str(a.records), str(a.ops),
                   str(a.size_mb), path, 'ub' if a.memory == 'ub' else 'file', str(spec), str(token), a.updates]
            cmd = [*prefix, 'timeout', '--signal=TERM', '--kill-after=5', str(a.timeout), *cmd]
            if i and a.host:
                # exec preserves the worker's exit status and avoids a masking tee pipeline.
                cmd = ['ssh', '-oBatchMode=yes', a.host, 'exec '+shlex.join(cmd)]
            commands.append(cmd)
        logs, procs = [], []
        event = dict(case=case, commands=commands, executable_sha256=digest,
                     memory=a.memory, ub_api=a.ub_api, shared_object=path, host=a.host, updates=a.updates, mapping_bits=a.mapping_bits,
                     workload_sha256=hashlib.sha256(spec.read_bytes()).hexdigest(), status='running', run_token=token)
        try:
            if a.ub_api == 'ubsm':
                print(f'UBSM object: {path}', flush=True)
                event['object_state'] = 'creation-attempted'
                (out/'results.json').write_text(json.dumps([*results, event], indent=2)+'\n')
                with (out/f'{case}-admin.log').open('w') as log:
                    subprocess.run([*prefix, 'timeout', str(a.timeout), str(build/'ub_admin'), 'create',
                                    path, str(a.size_mb), a.provider_host, str(a.provider_numa)],
                                   check=True, stdout=log, stderr=subprocess.STDOUT, timeout=a.timeout+10)
                event['object_state'] = 'created'
            for i, cmd in enumerate(commands):
                log = (out/f'{case}-{i}.log').open('w'); logs.append(log)
                procs.append(subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True))
            deadline = time.monotonic()+a.timeout
            while any(proc.poll() is None for proc in procs):
                if any(proc.poll() not in (None, 0) for proc in procs):
                    raise RuntimeError(f'{case}: worker failed; inspect logs')
                if time.monotonic() > deadline:
                    raise TimeoutError(f'{case}: worker timeout')
                time.sleep(.1)
            if any(proc.returncode for proc in procs):
                raise RuntimeError(f'{case}: worker failed')
            rows = []
            memlink_stub = False
            ubsm_stub = False
            for i in range(2):
                text = (out/f'{case}-{i}.log').read_text()
                memlink_stub |= 'MEMLINK_TEST_STUB' in text
                ubsm_stub |= 'UBSM_TEST_STUB' in text
                if a.memory == 'ub' and f'UB_CLOSED api={a.ub_api} id={i}' not in text:
                    raise RuntimeError('Worker did not confirm UB unmap/finalize; retain object')
                if a.memory == 'ub' and f'UB_CAS_PROBE id={i} ' not in text:
                    raise RuntimeError('Worker did not validate shared UB CAS storage')
                lines = [line for line in text.splitlines() if line.startswith('RESULT ')]
                if len(lines) != 1:
                    raise RuntimeError(f'{case}: missing RESULT for worker {i}')
                row = dict(part.split('=', 1) for part in lines[0].split()[1:])
                expected = 0 if system == 'rpc' and i == 0 else a.ops
                if row['system'] != system or int(row['cc']) != (opt == 'cc') or int(row['opt']) != (opt not in ('off', 'cc')) or int(row['completed']) != expected or int(row['errors']) or int(row['validation_records']) != a.records or int(row['peer_marker_checked']) != 1 or int(row['threads']) != a.threads[i]:
                    raise RuntimeError(f'{case}: incomplete/incorrect run: {row}')
                rows.append(row)
            if not a.host and rows[0]['pid'] == rows[1]['pid']:
                raise RuntimeError('workers did not run in distinct processes')
            if a.ub_api == 'ubsm':
                with (out/f'{case}-admin.log').open('a') as log:
                    subprocess.run([*prefix, 'timeout', str(a.timeout), str(build/'ub_admin'), 'destroy', path],
                                   check=True, stdout=log, stderr=subprocess.STDOUT, timeout=a.timeout+10)
                event['object_state'] = 'destroyed'
            event.update(status='passed', results=rows, memlink_test_stub=memlink_stub, ubsm_test_stub=ubsm_stub)
            print(f'{case}: PASS; PIDs {[r["pid"] for r in rows]}; operations {[r["completed"] for r in rows]}', flush=True)
        except BaseException as error:
            event.update(status='failed', error=str(error))
            if a.ub_api == 'ubsm':
                print(f'UBSM failure: object {path} may remain; stop both workers before manual cleanup.', flush=True)
            raise
        finally:
            for proc in procs:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
            for proc in procs:
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL); proc.wait()
            for log in logs: log.close()
            event['exit_codes'] = [proc.returncode for proc in procs]
            results.append(event)
            (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
            if a.memory == 'file': backing.unlink(missing_ok=True)

if __name__ == '__main__':
    main()
