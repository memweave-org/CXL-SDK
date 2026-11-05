#!/usr/bin/env python3
"""Check shell routing without SDK mappings, index operations, or measurements."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
EXPECTED={
    'bwtree': ('bwtree',0,0,'cache',0),
    'bwtree+opt': ('bwtree',1,0,'cache',0),
    'bwtree+rpc': ('bwtree',0,1,'cache',0),
    'clevelhash': ('clevelhash',0,0,'cache',0),
    'clevelhash+opt': ('clevelhash',1,0,'cache',0),
    'clevelhash+rpc': ('clevelhash',0,1,'cache',0),
    'sherman+nc-direct': ('sherman',0,0,'nc-direct',0),
    'sherman+nc-memcpy': ('sherman',0,0,'nc-memcpy',0),
}


class RoutingTests(unittest.TestCase):
    def test_batch_plan(self):
        result=subprocess.run(['bash',str(HERE/'run_all.sh'),'--dry-run'],
                              capture_output=True,text=True,check=True)
        plan=json.loads(result.stdout)
        self.assertEqual(plan['cases'],list(EXPECTED))
        self.assertEqual(plan['A_workload'],'100% read')
        self.assertEqual(plan['B_workload'],'90% read / 10% update')
        self.assertEqual(plan['sherman']['writers'],'B only, no RPC')

    def test_a_writable_is_rejected_before_build_or_run(self):
        for script,arguments in [('run.sh',[]),('run_all.sh',['--dry-run'])]:
            result=subprocess.run(['bash',str(HERE/script),*arguments],
                env=dict(os.environ,A_WRITABLE='1'),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('A is always read-only',result.stderr)

    def test_cases_and_legacy_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); tools=root/'tools'; tools.mkdir()
            for name in ('run.sh','config.sh'):
                shutil.copy(HERE/name,root/name)
            (root/'build.sh').write_text('''#!/bin/bash
set -eu
mkdir -p "$1/lib"
touch "$1/ycsbc"
printf '%s %s' "$OPT" "$YCSB_INDEXES" > "$1/build-routing"
''')
            (tools/'ldd').write_text('#!/bin/sh\nexit 0\n')
            (tools/'python3').write_text('#!'+sys.executable+'\n'+
                'import json,os,sys\nfrom pathlib import Path\n'+
                'Path(os.environ["YCSB_UB_OUTPUT"],"peer-args.json").write_text(json.dumps(sys.argv[1:]))\n')
            for script in tools.iterdir(): script.chmod(0o700)
            for case,expected in list(EXPECTED.items())+[('legacy',('bwtree',1,0,'cache',0))]:
                with self.subTest(case=case):
                    out=root/case; out.mkdir()
                    env=dict(os.environ,PATH=str(tools)+':'+os.environ['PATH'],
                        YCSB_UB_ROLE='A',YCSB_UB_CASE='bwtree' if case=='legacy' else case,
                        YCSB_UB_SUITE='0' if case=='legacy' else '1',
                        YCSB_UB_OUTPUT=str(out),OPT='1',A_WRITABLE='0')
                    for key in ('YCSB_UB_BUNDLE','YCSB_UB_FIXTURE','YCSB_UB_CONFIG'):
                        env.pop(key,None)
                    subprocess.run(['bash',str(root/'run.sh')],env=env,check=True,capture_output=True)
                    db,opt,rpc,access,writable=expected
                    index='clevel-hash' if db=='clevelhash' else db
                    self.assertEqual((out/'build-routing').read_text(),f'{opt} {index}')
                    args=json.loads((out/'peer-args.json').read_text())
                    def value(flag): return args[args.index(flag)+1]
                    self.assertEqual(value('--expected-opt'),str(opt))
                    self.assertEqual('--suite' in args,case!='legacy')
                    if case!='legacy':
                        self.assertEqual(value('--db'),db)
                        self.assertEqual(value('--rpc'),str(rpc))
                        self.assertEqual(value('--sherman-access'),access)
                        self.assertEqual(value('--a-writable'),str(writable))


if __name__=='__main__': unittest.main()
