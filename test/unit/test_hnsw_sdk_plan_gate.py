#!/usr/bin/env python3
"""Pin the switch that turns the ACE SDK source planner on, and the build-time note when it is off.

Without a planner an ACE build no longer fails, so nothing but these checks notices a build that
was meant to have the planner and silently lost it. All CPU, no CUDA:
  (a) src/cuvs_wrapper.cu includes cuvs_hnsw_sdk_plan.cuh;
  (b) preprocessing that header gives CUVS_HNSW_HAS_SDK_PLAN 1 only with a fake
      neighbors/detail/cagra/cagra_build.cuh on the include path AND _OPENMP defined, else 0
      (_OPENMP is defined by hand: -fopenmp is what defines it, and Apple clang has no -fopenmp);
  (c) the Makefile hands -I<source> and -fopenmp to nvcc when the variable is set;
  (d) `make -n` prints the one-line "guarantee is OFF" note when the GPU wrapper would be compiled
      without CUVS_SDK_SOURCE_INCLUDE, and prints nothing with the variable set or in a CPU shim build.
Planted defects in copies must fail these checks.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
CXX = os.environ.get('CXX', 'c++')
NOTE = 'guarantee is OFF'

PROBE = '''#include "cuvs_hnsw_sdk_plan.cuh"
#if CUVS_HNSW_HAS_SDK_PLAN
HAS_PLAN_IS_1
#else
HAS_PLAN_IS_0
#endif
'''


def has_plan(header_text, with_source, openmp):
    with tempfile.TemporaryDirectory(prefix='pgcuvs-plan-gate-') as tmp:
        d = Path(tmp)
        (d / 'src').mkdir()
        (d / 'src/cuvs_hnsw_sdk_plan.cuh').write_text(header_text)
        (d / 'probe.cpp').write_text(PROBE)
        argv = [CXX, '-std=c++17', '-E', '-P', '-I', str(d / 'src')]
        if with_source:
            fake = d / 'sdk/neighbors/detail/cagra'
            fake.mkdir(parents=True)
            (fake / 'cagra_build.cuh').write_text('/* stands in for the pinned SDK planner source */\n')
            argv += ['-I', str(d / 'sdk')]
        if openmp:
            argv += ['-D_OPENMP=201511']
        out = subprocess.run(argv + [str(d / 'probe.cpp')], capture_output=True, text=True)
        if out.returncode:
            raise RuntimeError('probe failed: ' + out.stderr[-400:])
        return 1 if 'HAS_PLAN_IS_1' in out.stdout else 0 if 'HAS_PLAN_IS_0' in out.stdout else None


def check_gate(header_text, wrapper_text, makefile_text):
    assert '#include "cuvs_hnsw_sdk_plan.cuh"' in wrapper_text, 'WRAPPER_INCLUDES_PLAN_HEADER'
    assert has_plan(header_text, True, True) == 1, 'PLAN_ON_WITH_SOURCE_AND_OPENMP'
    assert has_plan(header_text, False, True) == 0, 'PLAN_OFF_WITHOUT_SOURCE'
    assert has_plan(header_text, True, False) == 0, 'PLAN_OFF_WITHOUT_OPENMP'
    assert 'CUVS_SDK_PLAN_FLAGS = -I$(CUVS_SDK_SOURCE_INCLUDE) -Xcompiler=-fopenmp' in makefile_text, 'MAKEFILE_PASSES_SOURCE_AND_OPENMP'
    assert '-fopenmp' in makefile_text.split('CUVS_SDK_PLAN_LINK =', 1)[-1].split('\n', 1)[0], 'MAKEFILE_LINKS_OPENMP'
    with tempfile.TemporaryDirectory(prefix='pgcuvs-plan-make-') as tmp:
        mk = Path(tmp) / 'Makefile'
        mk.write_text(makefile_text)
        env = {k: v for k, v in os.environ.items() if k not in ('CUVS_SDK_SOURCE_INCLUDE', 'PGCUVS_CPU_SHIM')}

        def noted(*args):
            # -B: an up to date server binary in a built tree would leave no recipe to print.
            r = subprocess.run(['make', '-n', '-B', '-f', str(mk)] + list(args), cwd=root, env=env,
                               capture_output=True, text=True)
            return NOTE in r.stdout
        assert noted('src/cuvs_wrapper.o'), 'NOTE_WHEN_GPU_WRAPPER_WITHOUT_SOURCE'
        assert noted('server'), 'NOTE_ON_GPU_SERVER_BUILD'
        assert not noted('CUVS_SDK_SOURCE_INCLUDE=/nonexistent/cpp/src', 'src/cuvs_wrapper.o'), 'NO_NOTE_WITH_SOURCE'
        assert not noted('PGCUVS_CPU_SHIM=1', 'server'), 'NO_NOTE_IN_SHIM_BUILD'
        assert not noted('PGCUVS_CPU_SHIM=1'), 'NO_NOTE_IN_SHIM_DEFAULT_BUILD'
        assert not noted('test-hnsw-policy'), 'NO_NOTE_IN_CPU_TESTS'
        # The note must not reach the environment of recipe processes (the Makefile has a bare `export`).
        probe = Path(tmp) / 'probe.mk'
        probe.write_text("pgcuvs-print-env:\n\t@env | grep -c '^CUVS_SDK_PLAN_NOTE=' || true\n")
        r = subprocess.run(['make', '-s', '-f', str(mk), '-f', str(probe), 'pgcuvs-print-env'], cwd=root, env=env,
                           capture_output=True, text=True)
        assert r.stdout.strip() == '0', 'NOTE_NOT_EXPORTED:' + r.stdout.strip() + r.stderr[-200:]


MUTANTS = [
    ('M12_wrapper_drops_plan_header_include', 'wrapper',
     '#include "cuvs_hnsw_sdk_plan.cuh"', '/* no plan header */'),
    ('M10b_plan_gate_forced_off', 'header',
     '#if defined(_OPENMP) && __has_include', '#if 0 && defined(_OPENMP) && __has_include'),
    ('M10c_plan_gate_wrong_openmp_macro', 'header',
     '#if defined(_OPENMP) &&', '#if defined(_OPENMP_X) &&'),
    ('plan_gate_wrong_source_path', 'header',
     '#if defined(_OPENMP) && __has_include(<neighbors/detail/cagra/cagra_build.cuh>)',
     '#if defined(_OPENMP) && __has_include(<neighbors/detail/cagra/cagra_build2.cuh>)'),
    ('plan_gate_ignores_openmp', 'header',
     '#if defined(_OPENMP) && __has_include', '#if __has_include'),
    ('makefile_drops_openmp_flag', 'makefile',
     'CUVS_SDK_PLAN_FLAGS = -I$(CUVS_SDK_SOURCE_INCLUDE) -Xcompiler=-fopenmp',
     'CUVS_SDK_PLAN_FLAGS = -I$(CUVS_SDK_SOURCE_INCLUDE)'),
    ('makefile_note_removed', 'makefile', '\t$(CUVS_SDK_PLAN_NOTE)\n', ''),
    ('makefile_note_exported', 'makefile', 'unexport CUVS_SDK_PLAN_NOTE\n', ''),
    ('makefile_note_unconditional', 'makefile', '\t$(CUVS_SDK_PLAN_NOTE)\n',
     '\t@echo "pg_cuvs: ACE effective partition count guarantee is OFF"\n'),
    ('makefile_note_never_defined', 'makefile', 'CUVS_SDK_PLAN_NOTE = @echo', 'CUVS_SDK_PLAN_NOTE_UNUSED = @echo'),
]

if __name__ == '__main__':
    src = {'header': (root / 'src/cuvs_hnsw_sdk_plan.cuh').read_text(),
           'wrapper': (root / 'src/cuvs_wrapper.cu').read_text(),
           'makefile': (root / 'Makefile').read_text()}
    check_gate(src['header'], src['wrapper'], src['makefile'])
    print('plan gate: header included, plan on only with source + OpenMP, note only for a GPU wrapper without source: PASS')
    for name, key, old, new in MUTANTS:
        assert src[key].count(old) == 1, ('MUTANT_ANCHOR', name, src[key].count(old))
        m = dict(src); m[key] = src[key].replace(old, new, 1)
        try:
            check_gate(m['header'], m['wrapper'], m['makefile'])
        except AssertionError as e:
            print('mutation %s: detected (%s)' % (name, e))
        else:
            sys.exit('MUTANT_SURVIVED:' + name)
