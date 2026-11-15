#!/usr/bin/env python3
"""Compile and run the ACE branch of src/cuvs_wrapper.cu with and without the SDK source planner.

cuvs_wrapper.cu needs CUDA and cuVS, so this cuts the statements of the real ACE branch of
execute_hnsw_file_build (from the `ace_params` declaration to `cp.graph_build_params = ace;`)
and compiles them as C++17 against stubs of the few cuVS types they touch. Once with
CUVS_HNSW_HAS_SDK_PLAN 1 (a planner stub that returns chosen counts) and once with 0.
This does not compile CUDA, link cuVS or run a GPU; it shows what the two preprocessor
branches do to the receipt and to the count handed to the SDK. Planted defects in a copy of
the source must change the result.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
CXX = os.environ.get('CXX', 'c++')
FIRST = '            cagra::graph_build_params::ace_params ace;'
LAST = '            cp.graph_build_params = ace;'

HARNESS = r'''
#include "cuvs_hnsw_policy.h"
#include <cstdio>
#include <stdexcept>
namespace cagra { namespace graph_build_params {
struct ace_params { bool use_disk = false; uint32_t ef_construction = 0; const char *build_dir = nullptr;
                    size_t npartitions = 0; double max_host_memory_gb = 0, max_gpu_memory_gb = 0; };
} struct index_params { graph_build_params::ace_params graph_build_params; }; }
struct Params { uint64_t n_vecs; uint32_t dim; };
static size_t g_first, g_second; static int g_calls;
static inline size_t cuvs_hnsw_sdk_ace_plan(const cagra::index_params &, uint64_t, uint32_t, size_t,
                                            const char *, double, double)
{ return ++g_calls == 1 ? g_first : g_second; }
struct Out { size_t npartitions; uint32_t partitions; uint32_t flags; bool threw; };
static Out run(uint32_t flags_in, size_t requested, size_t first, size_t second)
{
    CuvsHnswPolicy receipt = {}; receipt.partitions = (uint32_t)requested; receipt.route_flags = flags_in;
    receipt.ace_ef = 240;
    const char *workdir = "/tmp/pgcuvs-ace-branch";
    const Params fixed = {999000, 8}, *params = &fixed; (void)params;
    const size_t partitions = requested;
    const uint64_t sdk_host_budget = UINT64_C(1) << 30, sdk_gpu_budget = UINT64_C(1) << 30;
    cagra::index_params cp; g_calls = 0; g_first = first; g_second = second;
    Out o = {0, 0, 0, false};
    try {
@BRANCH@
        o.npartitions = cp.graph_build_params.npartitions;
    } catch (const std::runtime_error &) { o.threw = true; }
    o.partitions = receipt.partitions; o.flags = receipt.route_flags; return o;
}
int main()
{
    const uint32_t F = CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;
    const uint32_t K = CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK;
#if CUVS_HNSW_HAS_SDK_PLAN
    (void)F; (void)K;
    Out e = run(0, 4, 6, 6);                 /* the plan is stable at 6: 6 is handed over and recorded */
    if (e.threw || e.npartitions != 6 || e.partitions != 6 || (e.flags & F)) return 10;
    e = run(K, 4, 6, 7);                     /* the plan moves between the two calls: no build */
    if (!e.threw) return 11;
    puts("planner: effective count recorded and handed to the SDK, unstable plan refused, receipt not marked");
#else
    Out e = run(0, 4, 99, 99);               /* no planner: the requested count goes through, marked */
    if (e.threw || e.npartitions != 4 || e.partitions != 4 || e.flags != F) return 20;
    e = run(K, 4, 99, 99);                   /* the other record bits survive */
    if (e.threw || e.partitions != 4 || e.flags != (F | K)) return 21;
    puts("no planner: requested count handed to the SDK and kept, receipt marked, other bits kept, no refusal");
#endif
    return 0;
}
'''


def build_and_run(wrapper_text, has_plan):
    start = wrapper_text.index(FIRST, wrapper_text.index('static int execute_hnsw_file_build('))
    end = wrapper_text.index(LAST, start) + len(LAST)
    with tempfile.TemporaryDirectory(prefix='pgcuvs-ace-branch-') as tmp:
        d = Path(tmp)
        (d / 't.cpp').write_text(HARNESS.replace('@BRANCH@', wrapper_text[start:end]))
        built = subprocess.run([CXX, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                '-DCUVS_HNSW_HAS_SDK_PLAN=%d' % has_plan, '-I', str(root / 'src'),
                                str(d / 't.cpp'), '-o', str(d / 't')], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError('branch does not compile with HAS_SDK_PLAN=%d: %s' % (has_plan, built.stderr[-800:]))
        ran = subprocess.run([str(d / 't')], capture_output=True, text=True)
        return ran.returncode, ran.stdout.strip()


MARK = '            receipt.route_flags |= CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;\n'
MUTANTS = [
    ('old_refusal_without_planner', 0, MARK,
     '            throw std::runtime_error("ACE metadata requires matching pinned SDK source planner; set CUVS_SDK_SOURCE_INCLUDE");\n', 20),
    ('no_planner_receipt_not_marked', 0, MARK, '', 20),
    ('no_planner_other_bits_cleared', 0, MARK,
     '            receipt.route_flags = CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;\n', 21),
    ('planner_receipt_marked', 1, '            receipt.partitions = (uint32_t)effective;\n',
     '            receipt.partitions = (uint32_t)effective;\n            receipt.route_flags |= CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;\n', 10),
    ('planner_stability_unchecked', 1, '            if (effective != rechecked)', '            if (false && effective != rechecked)', 11),
    ('planner_effective_not_handed_over', 1, '            ace.npartitions = effective;\n', '', 10),
]

if __name__ == '__main__':
    text = (root / 'src/cuvs_wrapper.cu').read_text()
    for has_plan in (1, 0):
        code, out = build_and_run(text, has_plan)
        print(out)
        if code:
            sys.exit(code)
    for name, has_plan, old, new, expected in MUTANTS:
        assert text.count(old) == 1, ('MUTANT_ANCHOR', name, text.count(old))
        code, out = build_and_run(text.replace(old, new, 1), has_plan)
        if code != expected:
            raise AssertionError('MUTANT_NOT_DETECTED:%s (exit %d, wanted %d)' % (name, code, expected))
        print('mutation %s: detected (exit %d)' % (name, code))
