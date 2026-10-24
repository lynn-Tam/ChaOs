#!/usr/bin/env python3
"""Configure one coherent, target-specific GCC gthread port; kernel stays stock."""
from pathlib import Path
import sys
config, output, backend = map(Path, sys.argv[1:])
headers = config.parent

def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)

text = (config / 'bits/c++config.h').read_text()
for macro in ('HAS_GTHREADS', 'HAVE_TLS', 'HAVE_ATOMIC_LOCK_POLICY', 'USE_CLOCK_MONOTONIC', 'USE_PTHREAD_COND_CLOCKWAIT'):
    key = '_GLIBCXX_' + macro
    old = f'/* #undef {key} */'
    if old not in text:
        raise SystemExit(f'unsupported GCC target configuration: {key}')
    text = text.replace(old, f'#define {key} 1')
text = '#define _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK 1\n' + text
write(output / 'bits/c++config.h', text)
write(output / 'bits/gthr-default.h', backend.read_text())
# GCC's clock-select extension is POSIX-specific; connect it to this target backend.
for source, symbol in [('bits/std_mutex.h', 'pthread_cond_clockwait'), ('mutex', 'pthread_mutex_clocklock')]:
    text = (headers / source).read_text()
    if symbol not in text:
        raise SystemExit(f'unsupported GCC clock extension: {source}')
    write(output / source, text.replace(symbol, '__gthread_' + symbol.removeprefix('pthread_')))
