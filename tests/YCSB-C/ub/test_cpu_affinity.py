#!/usr/bin/env python3
"""CPU selection and controller/native handoff checks; no benchmark workload."""
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from cpu_affinity import select_cpus, select_cxl_role_cpus
from peer import Worker

HERE = Path(__file__).resolve().parent


class AffinityTests(unittest.TestCase):
    def test_numa_selection_never_spills_into_other_nodes(self):
        node_cpus = {'0': '0-23,96-119', '1': '24-47,120-143'}
        def node_path(value):
            node = value.split('/node')[-1].split('/')[0]
            return SimpleNamespace(is_file=lambda: node in node_cpus,
                                   read_text=lambda: node_cpus[node])
        with patch('cpu_affinity.Path', side_effect=node_path):
            roles = select_cxl_role_cpus(48, range(192), {})
            self.assertEqual(roles['A'], list(range(24)) + list(range(96,120)))
            self.assertEqual(roles['B'], list(range(24,48)) + list(range(120,144)))
            with self.assertRaisesRegex(ValueError, 'CXL A on NUMA0.*available=48'):
                select_cxl_role_cpus(64, range(192), {})
            with self.assertRaisesRegex(ValueError, 'CXL A on NUMA0.*Start CPU 24'):
                select_cxl_role_cpus(1, range(192), {'CPU_A': '24'})
            with self.assertRaisesRegex(ValueError, 'no allowed CPUs'):
                select_cxl_role_cpus(1, range(48,96), {})
            swapped = select_cxl_role_cpus(1, range(192), {'CXL_CPU_NODE_A':'1','CXL_CPU_NODE_B':'0'})
            self.assertEqual(swapped, {'A':[24], 'B':[0]})

    def test_selection_respects_allowed_set_and_start(self):
        self.assertEqual(select_cpus(3, 2, {2, 7, 10, 20}), [2, 7, 10])
        self.assertEqual(select_cpus(2, 7, {2, 7, 10, 20}), [7, 10])
        with self.assertRaisesRegex(ValueError, 'Start CPU 3 is not allowed'):
            select_cpus(2, 3, {2, 7, 10})
        with self.assertRaisesRegex(ValueError, 'available=4, available at/above CPU 7=3'):
            select_cpus(4, 7, {2, 7, 10, 20})

    def test_controller_passes_the_same_sparse_list_to_taskset_and_worker(self):
        args = SimpleNamespace(role='A', binary='unused', threads_a=3, threads_b=3,
                               size_mb=128, nc_size_mb=128, records=2, ops=1, cpu=2,
                               spec='unused', ini='unused', timeout=1)
        with patch('peer.os.sched_getaffinity', return_value={2, 7, 10, 20}), \
             patch('peer.subprocess.Popen', side_effect=RuntimeError('captured before launch')) as spawn:
            with self.assertRaisesRegex(RuntimeError, 'captured before launch'):
                Worker(args, 'unused', 'unused', io.StringIO())
        self.assertEqual(spawn.call_args.args[0][:3], ['taskset', '-c', '2,7,10'])
        env = spawn.call_args.kwargs['env']
        self.assertEqual(env['YCSB_UB_CPUS'], '2,7,10')
        self.assertEqual(env['YCSB_UB_CPU'], '2')
        self.assertEqual(env['YCSB_UB_THREADS_A'], '3')

    def test_rpc_reserves_separate_client_and_service_cpus(self):
        args = SimpleNamespace(role='A', binary='unused', threads_a=2, threads_b=3,
                               size_mb=128, nc_size_mb=128, records=2, ops=1, cpu=2,
                               spec='unused', ini='unused', timeout=1, rpc=1)
        with patch('peer.os.sched_getaffinity', return_value={2, 7, 10, 20}), \
             patch('peer.subprocess.Popen', side_effect=RuntimeError('captured')) as spawn:
            with self.assertRaisesRegex(RuntimeError, 'captured'):
                Worker(args, 'unused', 'unused', io.StringIO())
        self.assertEqual(spawn.call_args.args[0][:3], ['taskset', '-c', '2,7,10,20'])
        env = spawn.call_args.kwargs['env']
        self.assertEqual(env['YCSB_UB_CPUS'], '2,7')
        self.assertEqual(env['YCSB_RPC_CPUS'], '10,20')

    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler unavailable')
    def test_native_parser_preserves_cpu_ids_and_rejects_bad_lists(self):
        source = r'''
#include "core/ub_cpu.h"
#include <cassert>
int main() {
  using ycsbc_ub::parse_worker_cpus;
  assert((parse_worker_cpus("2,7,10",3,2)==std::vector<int>{2,7,10}));
  assert((parse_worker_cpus(nullptr,3,2)==std::vector<int>{2,3,4}));
  for(const char* invalid:{"2,7", "2,7,7", "2,,10", "2,7,", "2,-7,10", "2,7,4096"}) {
    bool rejected=false;
    try { parse_worker_cpus(invalid,3,2); } catch(const std::exception&) { rejected=true; }
    assert(rejected);
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'check.cc').write_text(source)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(HERE.parent / 'include'), str(root / 'check.cc'),
                            '-o', str(root / 'check')], check=True)
            subprocess.run([str(root / 'check')], check=True)


if __name__ == '__main__':
    unittest.main()
