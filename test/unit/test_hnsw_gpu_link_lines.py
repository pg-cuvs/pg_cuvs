#!/usr/bin/env python3
"""The GPU link lines name librapids_logger once, whether or not the SDK source planner is on.

The wrapper expands RMM_LOG_*, which call rapids_logger::logger::log in librapids_logger.so;
libcuvs.so and librmm.so need it only indirectly and ld does not take symbols from a library's
own dependencies, so a GPU build that does not name -lrapids_logger fails to link with
"DSO missing from command line". It used to be named only inside the CUVS_SDK_SOURCE_INCLUDE
branch, so the build without the planner (the point of an optional planner) could not link.
Checked by expanding the Makefile's SHLIB_LINK and SERVER_LDFLAGS (no compiler, no GPU):
  GPU build, no source, with source, and with hand-written CUVS_SO_LINK / CUVS_SERVER_LINK:
  -lrapids_logger exactly once in each; -fopenmp only with the source; CPU shim: never.
Planted defects in a copy of the Makefile must fail.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
PROBE = 'pgcuvs-print-links:\n\t@echo "SHLIB:$(SHLIB_LINK)"\n\t@echo "SERVER:$(SERVER_LDFLAGS)"\n'


def links(makefile_text, *args):
    with tempfile.TemporaryDirectory(prefix='pgcuvs-links-') as tmp:
        mk, probe = Path(tmp) / 'Makefile', Path(tmp) / 'probe.mk'
        mk.write_text(makefile_text); probe.write_text(PROBE)
        env = {k: v for k, v in os.environ.items() if k not in ('CUVS_SDK_SOURCE_INCLUDE', 'PGCUVS_CPU_SHIM')}
        r = subprocess.run(['make', '-s', '-f', str(mk), '-f', str(probe), 'pgcuvs-print-links'] + list(args),
                           cwd=root, env=env, capture_output=True, text=True)
        out = dict(line.split(':', 1) for line in r.stdout.splitlines() if line.startswith(('SHLIB:', 'SERVER:')))
        if set(out) != {'SHLIB', 'SERVER'}:
            raise RuntimeError('cannot expand the link variables: ' + r.stdout[-300:] + r.stderr[-300:])
        return out


def check(makefile_text):
    def logger(out, n, label):
        for k in ('SHLIB', 'SERVER'):
            assert out[k].split().count('-lrapids_logger') == n, '%s:%s:%s' % (label, k, out[k])
    gpu = links(makefile_text)
    logger(gpu, 1, 'GPU_NO_SOURCE_LINKS_LOGGER')
    assert '-fopenmp' not in gpu['SHLIB'].split() and '-fopenmp' not in gpu['SERVER'].split(), 'NO_OPENMP_WITHOUT_SOURCE'
    src = links(makefile_text, 'CUVS_SDK_SOURCE_INCLUDE=/nonexistent/cpp/src')
    logger(src, 1, 'GPU_WITH_SOURCE_LOGGER_ONCE')
    assert '-fopenmp' in src['SHLIB'].split() and '-fopenmp' in src['SERVER'].split(), 'OPENMP_WITH_SOURCE'
    hand = links(makefile_text, 'CUVS_SO_LINK=-L/x -lcuvs', 'CUVS_SERVER_LINK=-L/x -lcuvs -lrmm')
    logger(hand, 1, 'HAND_WRITTEN_OVERRIDES_KEEP_LOGGER')
    for extra in ([], ['CUVS_SDK_SOURCE_INCLUDE=/nonexistent/cpp/src']):
        shim = links(makefile_text, 'PGCUVS_CPU_SHIM=1', *extra)
        logger(shim, 0, 'SHIM_NEVER_LINKS_LOGGER')
        assert '-lcuvs' not in shim['SHLIB'] and '-lcuvs' not in shim['SERVER'], 'SHIM_HAS_NO_CUVS'


MUTANTS = [
    ('logger_back_inside_plan_link',
     'CUVS_SDK_PLAN_LINK = -fopenmp\n', 'CUVS_SDK_PLAN_LINK = -fopenmp -lrapids_logger\n'),
    ('logger_not_in_extension_link',
     'SHLIB_LINK     = $(CUVS_SO_LINK) $(CUVS_LOGGER_LINK) $(CUVS_SDK_PLAN_LINK)',
     'SHLIB_LINK     = $(CUVS_SO_LINK) $(CUVS_SDK_PLAN_LINK)'),
    ('logger_not_in_daemon_link',
     'SERVER_LDFLAGS = $(CUVS_SERVER_LINK) $(CUVS_LOGGER_LINK) $(CUVS_SDK_PLAN_LINK)',
     'SERVER_LDFLAGS = $(CUVS_SERVER_LINK) $(CUVS_SDK_PLAN_LINK)'),
    ('logger_in_shim_build',
     'CUVS_SERVER_LINK = -lm\nCUVS_LOGGER_LINK =\n', 'CUVS_SERVER_LINK = -lm\nCUVS_LOGGER_LINK = -lrapids_logger\n'),
    ('logger_tied_to_hand_written_link',
     'CUVS_LOGGER_LINK = -lrapids_logger\n', 'CUVS_LOGGER_LINK = $(if $(findstring -lcuvs -lcudart,$(CUVS_SO_LINK)),-lrapids_logger)\n'),
]

if __name__ == '__main__':
    text = (root / 'Makefile').read_text()
    check(text)
    print('GPU link lines: -lrapids_logger once in the extension and daemon links with and without the planner source, never in the CPU shim: PASS')
    for name, old, new in MUTANTS:
        assert text.count(old) == 1, ('MUTANT_ANCHOR', name, text.count(old))
        try:
            check(text.replace(old, new, 1))
        except AssertionError as e:
            print('mutation %s: detected (%s)' % (name, str(e)[:70]))
        else:
            sys.exit('MUTANT_SURVIVED:' + name)
