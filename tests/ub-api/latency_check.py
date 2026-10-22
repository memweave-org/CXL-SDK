#!/usr/bin/env python3
"""Check that UB atomic requests cannot call the simulated latency clock."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ub-latency-') as tmp:
    source = Path(tmp) / 'probe.cc'
    source.write_text('''#include "utils/atomic_queued_variable.h"
void probe(void* p) {
  AtomicRequestQueue::pend_cas_req(p);
  AtomicRequestQueue::pend_load_req(p);
  AtomicRequestQueue::pend_store_req(p);
}
''')
    for mode, defines in [('none', []), ('memlink', ['-DSHM_LIB_WITH_UB']),
                          ('ubsm', ['-DSHM_LIB_WITH_UBSM'])]:
        obj = Path(tmp) / (mode + '.o')
        subprocess.run([*shlex.split(os.getenv('CXX', 'c++')), '-std=c++17',
                        '-O2', '-march=native', '-I' + str(ROOT / 'shm-lib/include'),
                        *defines, '-c', str(source), '-o', str(obj)], check=True)
        symbols = subprocess.check_output(['nm', '-u', str(obj)], text=True)
        uses_clock = 'clock_gettime' in symbols
        if uses_clock != (mode == 'none'):
            raise RuntimeError(f'{mode}: unexpected simulated clock dependency: {symbols}')
        print(f'{mode}: PASS simulated_latency={"on" if uses_clock else "off"}')
