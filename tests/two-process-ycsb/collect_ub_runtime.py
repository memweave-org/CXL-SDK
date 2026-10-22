#!/usr/bin/env python3
"""Stage the vendor SDK's resolved dependency closure, excluding the toolchain."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

# The image owns glibc, its loader and the GCC runtime. Mixing host copies of
# these with the image's compiler/runtime can make the entire container unusable.
CORE = re.compile(r'^(?:ld-linux.*|ld64.*|lib(?:c|m|mvec|pthread|dl|rt|resolv|anl|util|BrokenLocale|nss_[^/]+|thread_db)\.so(?:\..*)?|lib(?:stdc\+\+|gcc_s|atomic|gomp|quadmath)\.so(?:\..*)?)$')


def collect(library, output, extra):
    output.mkdir(parents=True, exist_ok=False)
    manifest = {}
    env = dict(os.environ, LC_ALL='C')
    # The caller supplies SDK/extra library directories via LD_LIBRARY_PATH.
    for seed in [library, *extra]:
        seed = seed.resolve(strict=True)
        result = subprocess.run(['ldd', str(seed)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / (seed.name + '.ldd.log')).write_text(result.stdout)
        if result.returncode or 'not found' in result.stdout:
            raise RuntimeError(f'Host cannot resolve SDK dependencies for {seed}:\n{result.stdout}')
        files = {seed.name: seed}
        for line in result.stdout.splitlines():
            match = re.match(r'\s*(\S+)\s+=>\s+(/.*?)\s+\(0x[0-9a-fA-F]+\)', line)
            if match:
                name, path = match.groups()
                if '/' in name:  # Absolute DT_NEEDED would bypass this bundle.
                    raise RuntimeError(f'Absolute dependency {name}; mount its path explicitly')
                files[name] = Path(path).resolve(strict=True)
        for name, source in files.items():
            if CORE.match(name):
                continue
            if name in manifest and manifest[name] != str(source):
                raise RuntimeError(f'Conflicting SDK libraries named {name}')
            if name not in manifest:
                shutil.copy2(source, output / name)
                manifest[name] = str(source)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'UB runtime bundle: {output} ({len(manifest)} libraries)', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--extra', action='append', default=[], type=Path,
                   help='SDK plugin loaded via dlopen; also collect its dependencies')
    args = p.parse_args()
    try:
        collect(args.library, args.output, args.extra)
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(2)
