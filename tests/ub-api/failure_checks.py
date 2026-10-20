#!/usr/bin/env python3
"""Run after the documented UBSM fixture build. Never loads a vendor SDK."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
library = root / 'build/ub-api-fixture/libubsm_sdk.so'
with tempfile.TemporaryDirectory(prefix='ubsm-errors-') as tmp:
    tmp = Path(tmp)
    for fault in ('map', 'unmap', 'finalize'):
        objects = tmp / (fault + '-objects')
        out = tmp / fault
        env = dict(os.environ, UBSM_TEST_DIRECTORY=str(objects), UBSM_TEST_FAIL=fault)
        cmd = [sys.executable, str(root/'tests/two-process-ycsb/run.py'),
               '--cases', 'sherman', '--memory', 'ub', '--ub-api', 'ubsm',
               '--ub-library', str(library), '--ub-include', str(root/'tests/ub-api/mock'),
               '--provider-host', 'test-owner', '--shared-path', 'fault-test',
               '--threads', '1', '2', '--records', '1000', '--ops', '2000',
               '--size-mb', '1024', '--mapping-bits', '16', '--skip-build',
               '--timeout', '20', '--output', str(out)]
        result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=40)
        assert result.returncode != 0, result.stdout
        event = json.loads((out/'results.json').read_text())[0]
        assert event['status'] == 'failed', event
        assert event['object_state'] != 'destroyed', event
        assert (objects/event['shared_object']).exists(), event
        admin_log = (out/'sherman-admin.log').read_text()
        assert 'UBSM_TEST_STUB' in admin_log, 'This test requires the test SDK'
        assert 'UB_OBJECT_DESTROYED' not in admin_log, admin_log
        print(f'PASS: {fault} failure retains the exact object, without fallback or deallocation')
