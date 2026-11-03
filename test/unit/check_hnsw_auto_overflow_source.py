"""I3-C source-structure invariant (CK-17 (13), (14); UC-5 sub-decision 4).

Replaces I1's NO_SILENT_AUTO_ACE / EXPLICIT_NOTICE scans with:
  "auto -> ACE goes through one policy branch only and always leaves a
   NOTICE and a receipt", plus the decision order of ADR-087 decision 4:
   lock -> route decision -> mkdtemp -> disk check -> build (daemon) and
   statistics forecast -> heap scan -> build op -> NOTICEs (backend).
CPU source conformance only; it does not compile CUDA or run PG/GPU.
Every check is paired with a source mutant that must be detected.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_hnsw_explicit_source import function  # noqa: E402

FILES = {
    'common': 'src/cuvs_hnsw_build_common.h',
    'preflight': 'src/cuvs_route_preflight.h',
    'policy': 'src/cuvs_hnsw_policy.h',
    'wrapper': 'src/cuvs_wrapper.cu',
    'server': 'src/pg_cuvs_server.c',
    'backend': 'src/pg_cuvs.c',
}


def strip_comments(s):
    # Drop comments but keep string/char literals (e.g. strstr(path, "//")).
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: m[0] if m[0][0] in '"\'' else '', s, flags=re.S)


def ordered(body, needles, label):
    pos = []
    for n in needles:
        assert n in body, label + ':MISSING:' + n
        pos.append(body.index(n))
    assert pos == sorted(pos), label + ':ORDER'


def check(src):
    code = {k: strip_comments(v) for k, v in src.items()}
    # --- single overflow branch -------------------------------------------
    allcode = ''.join(code.values())
    assert len(re.findall(r'overflow\s*==\s*CUVS_HNSW_OVERFLOW_ACE', allcode)) == 1, 'ONE_OVERFLOW_TEST'
    assert len(re.findall(r'->overflowed\s*=|\.overflowed\s*=\s*\(', allcode)) == 1, 'ONE_OVERFLOW_WRITER'
    assert len(re.findall(r'route\s*=\s*CUVS_HNSW_ROUTE_ACE\s*;', allcode)) == 1, 'ONE_ACE_ROUTE_WRITER'
    resolver = function(code['common'], 'cuvs_hnsw_resolve_route(uint64_t n')
    assert 'algo == CUVS_HNSW_ALGO_AUTO' in resolver.split('overflow_to_ace =', 1)[1].split(';', 1)[0], 'OVERFLOW_ONLY_FOR_AUTO'
    assert 'out->overflowed = (uint32_t)overflow_to_ace;' in resolver, 'OVERFLOW_RECORDED'
    # the default macro is ace (UC-5)
    assert re.search(r'#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ACE\b', src['policy']), 'DEFAULT_ACE'
    # receipt: the wrapper records the branch, the contract requires it
    assert 'receipt.route_flags = prepared.route.overflowed ? CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW : 0;' in code['wrapper'], 'RECEIPT_FLAG'
    valid = function(code['policy'], 'static inline int cuvs_hnsw_policy_valid(')
    assert '(!!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW)' in valid, 'RECEIPT_CONTRACT'
    # every receipt path calls the resolver with the daemon flag
    pub = function(code['wrapper'], 'cuvs_hnsw_build_file(int vectors_fd')
    assert 'params->build_algo, params->auto_overflow, &cap,' in pub, 'WRAPPER_PASSES_FLAG'
    # --- daemon order (ADR-087 decision 4) ---------------------------------
    h = function(code['server'], 'handle_build_hnsw_file(int client_fd')
    ordered(h, ['cuvs_hnsw_wait_for_idle(', 'params.gpu_budget = hnsw_gpu_budget(device, 1);',
                'cuvs_hnsw_forecast_route(', 'hnsw_route_log("build"', 'mkdtemp(workdir)',
                'cuvs_hnsw_forecast_disk(&route', 'cuvs_hnsw_build_file('], 'DAEMON_ORDER')
    assert 'params.auto_overflow = g_hnsw_auto_overflow;' in h, 'DAEMON_FLAG'
    assert 'if (reply.status != CUVS_STATUS_OK) goto done;' in h.split('hnsw_route_log("build"', 1)[1].split('mkdtemp', 1)[0], 'DECISION_STOPS'
    assert 'wire.policy.route != route.decision.route' in h, 'RECEIPT_EQUALS_ROUTE_LOG'
    pre = function(code['server'], 'handle_hnsw_route_preflight(int client_fd)')
    ordered(pre, ['cuvs_hnsw_forecast_route(', 'cuvs_hnsw_forecast_disk(', 'hnsw_route_log(request.after_scan ? "postscan" : "prespool"'], 'PREFLIGHT_ORDER')
    assert 'mkdtemp' not in pre and 'wait_for_idle' not in pre, 'PREFLIGHT_READ_ONLY'
    # --- backend order (CK-17 (14)) -----------------------------------------
    b = function(code['backend'], 'cuvs_build_hnsw_from_heap(Relation heapRel')
    ordered(b, ['cuvs_hnsw_preflight(stats_rows', 'table_index_build_scan(',
                'prespool == CUVS_HNSW_PRESPOOL_FALLBACK', 'cuvs_ipc_build_hnsw_file(',
                'cuvs_hnsw_timing_flags(stats_rows, prespool)', 'cuvs_hnsw_build_notices(&out->policy, transition_sent)'],
            'BACKEND_ORDER')
    assert 'heapRel->rd_rel->reltuples' in b.split('table_index_build_scan(', 1)[0], 'STATS_BEFORE_SCAN'
    assert 'cuvs_hnsw_after_scan_reason(stats_rows)' in b, 'AFTER_SCAN_REASON_FROM_STATISTICS'
    notices = b.split('cuvs_hnsw_build_notices(', 1)[1]
    assert 'notices & CUVS_HNSW_NOTICE_TRANSITION' in notices and 'cuvs_hnsw_transition_notice(' in notices, 'POST_TRANSITION_NOTICE'
    assert 'auto selected ACE-disk' in notices.split('CUVS_HNSW_NOTICE_AUTO_COMPLETION', 1)[1].split(';', 1)[0] + notices, 'COMPLETION_NOTICE'
    p = function(code['backend'], 'cuvs_hnsw_preflight(double rows')
    assert 'action == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW' in p and 'cuvs_hnsw_transition_notice(' in p.split('CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW)', 1)[1], 'PN1_NOTICE'
    t = function(code['backend'], 'cuvs_hnsw_transition_notice(uint64_t host_est')
    assert 'build_algo=auto exceeded the resident budget (need %u MiB, budget %u MiB); building with ACE on disk' in src['backend'], 'TRANSITION_TEXT'
    assert 'ereport(NOTICE' in t, 'TRANSITION_IS_NOTICE'
    # CAGRA reltuples AR0 before any scan
    c = function(code['backend'], 'cuvs_build_cagra_from_heap(Relation heapRel')
    ordered(c, ['cuvs_cagra_prespool_ar0(', 'cuvs_build_parallel(', 'table_index_build_scan('], 'CAGRA_AR0_BEFORE_SCAN')
    return 'PASS_SOURCE_INVARIANT_NOT_CUDA'


MUTANTS = [
    ('default_reverted_to_error', 'policy',
     '#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ACE', '#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ERROR'),
    ('second_overflow_branch_in_daemon', 'server',
     '    params.auto_overflow = g_hnsw_auto_overflow;',
     '    params.auto_overflow = g_hnsw_auto_overflow;\n    if (g_hnsw_auto_overflow == CUVS_HNSW_OVERFLOW_ACE && params.build_algo == CUVS_HNSW_ALGO_AUTO && 0) params.build_algo = CUVS_HNSW_ALGO_ACE;'),
    ('receipt_without_overflow_record', 'wrapper',
     'receipt.route_flags = prepared.route.overflowed ? CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW : 0;', 'receipt.route_flags = 0;'),
    ('transition_without_notice', 'backend',
     '            if (notices & CUVS_HNSW_NOTICE_TRANSITION)\n                cuvs_hnsw_transition_notice(',
     '            if (0)\n                (void)('),
    ('pn1_notice_dropped', 'backend',
     '    if (action == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW)\n        cuvs_hnsw_transition_notice(',
     '    if (action == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW)\n        (void)('),
    ('decision_after_spool', 'backend',
     '        prespool = cuvs_hnsw_preflight(stats_rows,', '        prespool = CUVS_HNSW_PRESPOOL_FALLBACK; (void)(stats_rows,'),
    ('daemon_decision_after_mkdtemp', 'server',
     '    CuvsHnswForecast route;\n', '    CuvsHnswForecast route; (void)mkdtemp(workdir);\n'),
    ('daemon_disk_check_removed', 'server',
     '        if (cuvs_hnsw_forecast_disk(&route, regular, avail, reply.error,', '        if (0 && (regular, avail, reply.error,'),
    ('route_log_receipt_mismatch_ignored', 'server',
     '        && (wire.policy.route != route.decision.route', '        && 0 && (wire.policy.route'),
    ('timing_flags_ignore_statistics', 'backend',
     'out->policy.route_flags |= cuvs_hnsw_timing_flags(stats_rows, prespool);',
     'out->policy.route_flags |= cuvs_hnsw_timing_flags(0, prespool);'),
    ('after_scan_reason_from_row_count', 'backend',
     'cuvs_hnsw_after_scan_reason(stats_rows),', 'cuvs_prespool_rows(stats_rows) ? "pre-scan forecast unavailable" : "no table statistics",'),
    ('cagra_ar0_after_scan', 'backend',
     '            && cuvs_cagra_prespool_ar0(reltuples_est,', '            && 0 && (reltuples_est,'),
]


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    src = {k: (ROOT / v).read_text() for k, v in FILES.items()}
    result = dict(status=check(src), mutants=[], count=0)
    rows = result['mutants']
    for name, key, old, new in MUTANTS:
        assert src[key].count(old) == 1, ('MUTANT_ANCHOR', name, src[key].count(old))
        m = dict(src); m[key] = src[key].replace(old, new, 1)
        try:
            check(m)
        except (AssertionError, ValueError) as e:
            rows.append(dict(name=name, status='PASS_SOURCE_MUTANT_DETECTED', reason=str(e)))
        else:
            raise AssertionError('SOURCE_MUTANT_SURVIVED:' + name)
    result['count'] = len(rows)
    (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    a = argparse.ArgumentParser(); a.add_argument('output'); main(a.parse_args().output)
