#!/usr/bin/env python3
"""Check compile-time NT backend selection without measuring performance."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ub-latency-') as tmp:
    source = Path(tmp) / 'probe.cc'
    source.write_text('''#include "utils/atomic_variable.h"
#include "utils/atomic_pointer.h"
#ifdef SHM_NT_UB
static_assert(std::is_same<nt<uint64_t>, ub_nt<uint64_t>>::value);
#endif
uint64_t probe(nt<uint64_t>& value, aligned_nt<uint64_t>& aligned,
               nt_pointer<uint64_t>& pointer, nt_pointer<uint64_t[]>& array) {
  uint64_t expected = value.load();
  value.store(expected + 1);
  value.compare_exchange_strong(expected, 2);
  auto old = value.fetch_add(1) + aligned.fetch_sub(1);
  pointer.store(array.load());
  pointer.compare_exchange_strong(nullptr, array.load());
  return old;
}
''')
    for mode, defines in [('none', []), ('memlink', ['-DSHM_LIB_WITH_UB']),
                          ('ubsm', ['-DSHM_LIB_WITH_UBSM']),
                          ('ubsm-split', ['-DSHM_LIB_WITH_UBSM','-DSHM_UB_SPLIT']),
                          ('ubsm-stale-sim-flags', ['-DSHM_LIB_WITH_UBSM',
                                                   '-DQUEUE_SIM_CAS', '-DNT_SIM'])]:
        obj = Path(tmp) / (mode + '.o')
        subprocess.run([*shlex.split(os.getenv('CXX', 'c++')), '-std=c++17',
                        '-O2', '-march=native', '-DNO_CC', '-I' + str(ROOT / 'shm-lib/include'),
                        *defines, '-c', str(source), '-o', str(obj)], check=True)
        symbols = subprocess.check_output(['nm', '-u', str(obj)], text=True)
        uses_clock = 'clock_gettime' in symbols
        uses_queue = 'AtomicRequestQueue' in symbols
        if uses_clock != (mode == 'none') or uses_queue != (mode == 'none'):
            raise RuntimeError(f'{mode}: unexpected simulated clock dependency: {symbols}')
        print(f'{mode}: PASS simulated_latency={"on" if uses_clock else "off"}, queue={uses_queue}')
