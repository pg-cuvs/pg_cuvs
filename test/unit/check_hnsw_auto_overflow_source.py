"""I3-C source-structure invariant (CK-17 (13), (14); UC-5 sub-decision 4).

Replaces I1's NO_SILENT_AUTO_ACE / EXPLICIT_NOTICE scans with:
  "auto -> ACE goes through the named policy branches only and each always
   leaves a NOTICE and a receipt flag", plus the decision order of ADR-087 decision 4:
   lock -> route decision -> mkdtemp -> disk check -> build (daemon) and
   statistics forecast -> heap scan -> build op -> NOTICEs (backend).
The branches are (1) the resolver's budget overflow (UC-5) and (2) the R1
kNN-graph fallback (217d29b): auto chose resident IVF-PQ, cuVS rejected its
kNN graph, overflow=ace rebuilds on ACE-disk. Each is gated on auto and
overflow=ace and has its own receipt flag (AUTO_OVERFLOW, GRAPH_FALLBACK).
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
    # --- auto -> ACE branches: one overflow test and one ACE writer each ---
    allcode = ''.join(code.values())
    overflow_test = r'overflow\s*==\s*CUVS_HNSW_OVERFLOW_ACE'
    ace_writer = r'route\s*=\s*CUVS_HNSW_ROUTE_ACE\s*;'
    resolver = function(code['common'], 'cuvs_hnsw_resolve_route(uint64_t n')
    gate = function(code['common'], 'static inline int cuvs_hnsw_graph_failure_to_ace(')
    pub = function(code['wrapper'], 'cuvs_hnsw_build_file(int vectors_fd')
    fallback = pub.split('if (!cuvs_hnsw_graph_failure_to_ace(', 1)
    fallback = fallback[1].split('catch (const std::bad_alloc &e)', 1)[0] if len(fallback) == 2 else ''
    assert len(re.findall(overflow_test, allcode)) == 2, 'ONE_OVERFLOW_TEST'
    assert len(re.findall(overflow_test, resolver)) == 1, 'ONE_OVERFLOW_TEST:RESOLVER'
    assert len(re.findall(overflow_test, gate)) == 1, 'ONE_OVERFLOW_TEST:GRAPH_FALLBACK'
    assert len(re.findall(r'->overflowed\s*=|\.overflowed\s*=\s*\(', allcode)) == 1, 'ONE_OVERFLOW_WRITER'
    assert len(re.findall(ace_writer, allcode)) == 2, 'ONE_ACE_ROUTE_WRITER'
    assert len(re.findall(ace_writer, resolver)) == 1, 'ONE_ACE_ROUTE_WRITER:RESOLVER'
    assert len(re.findall(ace_writer, fallback)) == 1, 'ONE_ACE_ROUTE_WRITER:GRAPH_FALLBACK'
    # R1 kNN-graph fallback (217d29b): auto + resident IVF-PQ + overflow=ace only,
    # only on the cuVS kNN-graph rejection, recorded and announced.
    assert gate.split('return', 1)[1].lstrip().startswith(
        'requested_algo == CUVS_HNSW_ALGO_AUTO && route == CUVS_HNSW_ROUTE_IVF_PQ'), 'GRAPH_FALLBACK_ONLY_FOR_AUTO_IVF_PQ'
    assert 'strstr(what, CUVS_HNSW_GRAPH_KNN_REJECTED)' in gate, 'GRAPH_FALLBACK_ONLY_ON_KNN_REJECTION'
    assert re.search(r'if \(!cuvs_hnsw_graph_failure_to_ace\(params->build_algo, prepared\.route\.route,\s*'
                     r'params->auto_overflow, params->n_vecs, e\.what\(\)\)\)\s*throw;', pub), 'GRAPH_FALLBACK_GATED'
    assert '| CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK;' in fallback.split('return execute_hnsw_file_build(', 1)[0], 'GRAPH_FALLBACK_RECORDED'
    valid = function(code['policy'], 'static inline int cuvs_hnsw_policy_valid(')
    assert '+ !!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK)' in valid, 'RECEIPT_CONTRACT_GRAPH_FALLBACK'
    bn = function(code['preflight'], 'cuvs_hnsw_build_notices(const CuvsHnswPolicy *p')
    assert 'return CUVS_HNSW_NOTICE_GRAPH_FALLBACK;' in bn, 'GRAPH_FALLBACK_NOTICE_SELECTED'
    assert re.search(r'if \(notices & CUVS_HNSW_NOTICE_GRAPH_FALLBACK\)\s*ereport\(NOTICE', code['backend']), 'GRAPH_FALLBACK_NOTICE'
    exempt = function(code['server'], 'handle_build_hnsw_file(int client_fd').split('int graph_fallback =', 1)
    assert len(exempt) == 2 and all(x in exempt[1].split(';', 1)[0] for x in (
        'route.decision.route == CUVS_HNSW_ROUTE_IVF_PQ', '!route.decision.overflowed',
        'wire.policy.route == CUVS_HNSW_ROUTE_ACE')), 'GRAPH_FALLBACK_EXEMPTION_NARROW'
    assert 'algo == CUVS_HNSW_ALGO_AUTO' in resolver.split('overflow_to_ace =', 1)[1].split(';', 1)[0], 'OVERFLOW_ONLY_FOR_AUTO'
    assert 'out->overflowed = (uint32_t)overflow_to_ace;' in resolver, 'OVERFLOW_RECORDED'
    # the default macro is ace (UC-5)
    assert re.search(r'#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ACE\b', src['policy']), 'DEFAULT_ACE'
    # receipt: the wrapper records the branch, the contract requires it
    assert 'receipt.route_flags = prepared.route.overflowed ? CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW : 0;' in code['wrapper'], 'RECEIPT_FLAG'
    assert '(!!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW)' in valid, 'RECEIPT_CONTRACT'
    # every receipt path calls the resolver with the daemon flag
    assert 'params->build_algo, params->auto_overflow, &cap,' in pub, 'WRAPPER_PASSES_FLAG'
    # v6 C2: bad_alloc (incl. RMM out_of_memory) -> status 2 before the generic catch
    full = code['wrapper'][code['wrapper'].index('cuvs_hnsw_build_file(int vectors_fd'):]
    # The outer handlers (4-space indent); the R1 fallback's inner std::exception
    # handler rethrows anything but the kNN-graph rejection (GRAPH_FALLBACK_GATED).
    ba = full.index('\n    catch (const std::bad_alloc &e)'); ex = full.index('\n    catch (const std::exception &e)')
    assert ba < ex and 'return CUVS_STATUS_OOM_FALLBACK;' in full[ba:ex], 'HNSW_BAD_ALLOC_IS_STATUS_2'
    # ACE partitions stay on the SDK's bounded NN-descent builder (memory-guard
    # patch 0003): the disk route requires its marker and passes the positive
    # SDK budgets, so cuVS's fp16 IVF-PQ partition builder is never selected.
    assert '"pgcuvs_hnsw_memory_guard_patch_id"' in pub.split('if (disk)', 1)[1].split('receipt.magic', 1)[0], 'ACE_REQUIRES_MEMORY_GUARD_SDK'
    ex = function(code['wrapper'], 'static int execute_hnsw_file_build(')
    for needle in ('ace.max_host_memory_gb = (double)sdk_host_budget / (UINT64_C(1) << 30);',
                   'ace.max_gpu_memory_gb = (double)sdk_gpu_budget / (UINT64_C(1) << 30);'):
        assert needle in ex, 'ACE_SDK_BUDGETS_PASSED:' + needle
    # --- daemon order (ADR-087 decision 4) ---------------------------------
    h = function(code['server'], 'handle_build_hnsw_file(int client_fd')
    ordered(h, ['cuvs_hnsw_wait_for_idle(', 'params.gpu_budget = hnsw_gpu_budget(device, 1, &wire.budget.logical_used);',
                'cuvs_hnsw_forecast_route(', 'wire.budget.host_estimate = route.host_estimate;',
                'hnsw_route_log("build"', 'mkdtemp(workdir)',
                'cuvs_hnsw_forecast_disk(&route', 'cuvs_hnsw_build_file('], 'DAEMON_ORDER')
    # v5 C5: the build reply carries the status-13 DETAIL bytes (he hb ge gb rp rl)
    for needle in ('wire.budget.pool_used = hnsw_pool_used(device);',
                   'wire.budget.host_budget = route.host_budget;',
                   'wire.budget.gpu_estimate = route.gpu_estimate;',
                   'wire.budget.gpu_budget = route.gpu_budget;'):
        assert needle in h, 'DAEMON_BUDGET_FILLED:' + needle
    assert 'params.auto_overflow = g_hnsw_auto_overflow;' in h, 'DAEMON_FLAG'
    assert 'if (reply.status != CUVS_STATUS_OK) goto done;' in h.split('hnsw_route_log("build"', 1)[1].split('mkdtemp', 1)[0], 'DECISION_STOPS'
    assert 'wire.policy.route != route.decision.route' in h, 'RECEIPT_EQUALS_ROUTE_LOG'
    pre = function(code['server'], 'handle_hnsw_route_preflight(int client_fd)')
    ordered(pre, ['cuvs_hnsw_forecast_route(', 'cuvs_hnsw_forecast_disk(', 'hnsw_route_log(request.after_scan ? "postscan" : "prespool"'], 'PREFLIGHT_ORDER')
    assert 'mkdtemp' not in pre and 'wait_for_idle' not in pre, 'PREFLIGHT_READ_ONLY'
    assert 'hnsw_gpu_budget(device, locked, &reply.logical_used)' in pre, 'PREFLIGHT_RL'
    # Q4-4: the route field is the decided route (also after a disk refusal)
    log = function(code['server'], 'hnsw_route_log(const char *phase')
    assert 'cuvs_hnsw_forecast_route_name(f)' in log and 'f->status == CUVS_STATUS_OK' not in log, 'ROUTE_LOG_DECIDED_ROUTE'
    assert 'reply.pool_used = hnsw_pool_used(device);' in pre, 'PREFLIGHT_RP'
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
    oom = b.split('if (rc == CUVS_STATUS_OOM_FALLBACK)', 1)
    seg = oom[1].split('if (rc != CUVS_STATUS_OK)', 1)[0] if len(oom) == 2 else ''
    assert 'errcode(ERRCODE_OUT_OF_MEMORY)' in seg and 'cuvs_hnsw_oom_hint(build_algo)' in seg, 'HNSW_OOM_53200'
    assert b.index('if (rc == CUVS_STATUS_OOM_FALLBACK)') < b.index('if (rc != CUVS_STATUS_OK)'), 'HNSW_OOM_BEFORE_GENERIC'
    # v5 C5 / v6 C5: status-13 DETAIL is exactly the six MiB fields
    be = function(code['backend'], 'cuvs_hnsw_budget_error(uint32_t build_algo')
    assert 'cuvs_hnsw_budget_detail(detail, sizeof(detail), budget)' in be and 'errdetail("%s", detail)' in be, 'STATUS13_DETAIL_SIX_FIELDS'
    assert be.count('errdetail(') == 1, 'STATUS13_DETAIL_ONLY_FIELDS'
    assert 'cuvs_hnsw_budget_error(build_algo, &budget);' in b, 'POSTSCAN_STATUS13_BUDGET'
    assert '&out->policy, &budget,' in b, 'POSTSCAN_BUDGET_RECEIVED'
    # v5 C5 / Q4-4: a disk refusal is the message itself, with the disk HINT
    post = b.split('if (rc == CUVS_STATUS_OOM_FALLBACK)', 1)[1]
    disk = post.split('if (cuvs_hnsw_disk_refusal(rc, error))', 1)
    assert len(disk) == 2 and post.index('if (cuvs_hnsw_disk_refusal(rc, error))') < post.index('if (rc != CUVS_STATUS_OK)'), 'POSTSCAN_DISK_BEFORE_GENERIC'
    seg = disk[1].split(';', 1)[0] + disk[1].split('if (rc != CUVS_STATUS_OK)', 1)[0]
    assert 'errmsg("%s", error)' in seg and 'errhint("%s", CUVS_HNSW_DISK_HINT)' in seg, 'POSTSCAN_DISK_SURFACE'
    p = function(code['backend'], 'cuvs_hnsw_preflight(double rows')
    rb = p.split('action == CUVS_HNSW_PRESPOOL_REJECT_BUDGET', 1)[1].split('return action;', 1)[0]
    assert 'cuvs_hnsw_preflight_budget(&reply)' in rb.split('cuvs_hnsw_budget_error(build_algo, &budget);', 1)[0], 'PRESPOOL_STATUS13_BUDGET'
    pd = p.split('cuvs_hnsw_disk_refusal(rc, reply.header.error)', 1)
    assert len(pd) == 2 and 'errmsg("%s", reply.header.error)' in pd[1].split(';', 1)[0] + pd[1].split('if (action == CUVS_HNSW_PRESPOOL_REJECT)', 1)[0], 'PRESPOOL_DISK_MESSAGE'
    assert 'errhint("%s", CUVS_HNSW_DISK_HINT)' in pd[1].split('if (action == CUVS_HNSW_PRESPOOL_REJECT)', 1)[0], 'PRESPOOL_DISK_HINT'
    rj = pd[1].split('if (action == CUVS_HNSW_PRESPOOL_REJECT)', 1)[1].split('if (action == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW)', 1)[0]
    assert 'cuvs_hnsw_budget_detail(detail, sizeof(detail), &budget)' in rj and 'errdetail("%s", detail)' in rj, 'PRESPOOL_STATUS5_DETAIL'
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
    ('hnsw_bad_alloc_generic', 'wrapper',
     '        return CUVS_STATUS_OOM_FALLBACK;\n    }\n    catch (const std::exception &e)',
     '        return CUVS_STATUS_BUILD_FAILED;\n    }\n    catch (const std::exception &e)'),
    ('hnsw_oom_not_53200', 'backend',
     '            if (rc == CUVS_STATUS_OOM_FALLBACK) /* v6 C2: allocation failure after admission */',
     '            if (0) /* v6 C2: allocation failure after admission */'),
    ('status13_detail_carries_reason_text', 'backend',
     '        errdetail("%s", detail),\n        build_algo == CUVS_HNSW_ALGO_AUTO',
     '        errdetail("%s; decided.", detail),\n        build_algo == CUVS_HNSW_ALGO_AUTO'),
    ('postscan_status13_without_budget', 'backend',
     '                cuvs_hnsw_budget_error(build_algo, &budget);\n            if (rc == CUVS_STATUS_CANCELED)',
     '                cuvs_hnsw_budget_error(build_algo, &(CuvsHnswBudgetSnapshot){0});\n            if (rc == CUVS_STATUS_CANCELED)'),
    ('prespool_disk_refusal_prefixed', 'backend',
     '                        errmsg("%s", reply.header.error),\n                        errdetail("Decided %s; no rows were spooled.", when),',
     '                        errmsg("pg_cuvs: temporary HNSW build failed (status %d): %s", rc, reply.header.error),\n                        errdetail("Decided %s; no rows were spooled.", when),'),
    ('prespool_disk_hint_dropped', 'backend',
     '                        errdetail("Decided %s; no rows were spooled.", when),\n                        errhint("%s", CUVS_HNSW_DISK_HINT)));',
     '                        errdetail("Decided %s; no rows were spooled.", when)));'),
    ('postscan_disk_refusal_generic', 'backend',
     '            if (cuvs_hnsw_disk_refusal(rc, error)) /* v5 C5: the daemon\'s workdir check */',
     '            if (0) /* v5 C5: the daemon\'s workdir check */'),
    ('prespool_status5_detail_bytes', 'backend',
     '                        errdetail("%s", detail),\n                        errhint("The forecast ran %s',
     '                        errdetail("he=%llu", (unsigned long long)reply.host_estimate),\n                        errhint("The forecast ran %s'),
    ('daemon_rp_not_sent', 'server',
     '    wire.budget.pool_used = hnsw_pool_used(device);\n', ''),
    ('daemon_estimates_not_sent', 'server',
     '    wire.budget.gpu_budget = route.gpu_budget;\n', ''),
    ('preflight_rl_not_sent', 'server',
     '    uint64_t gpu = hnsw_gpu_budget(device, locked, &reply.logical_used);',
     '    uint64_t unused_rl; uint64_t gpu = hnsw_gpu_budget(device, locked, &unused_rl);'),
    ('preflight_rp_not_sent', 'server',
     '    reply.pool_used = hnsw_pool_used(device);\n', ''),
    ('route_log_status_gated', 'server',
     '        cuvs_hnsw_forecast_route_name(f),',
     '        f->status == CUVS_STATUS_OK ? cuvs_hnsw_route_name(f->decision.route) : "none",'),
    ('cagra_ar0_after_scan', 'backend',
     '            && cuvs_cagra_prespool_ar0(reltuples_est,', '            && 0 && (reltuples_est,'),
    # R1 kNN-graph fallback (217d29b)
    ('graph_fallback_for_explicit_ivf_pq', 'common',
     '    return requested_algo == CUVS_HNSW_ALGO_AUTO && route == CUVS_HNSW_ROUTE_IVF_PQ',
     '    return route == CUVS_HNSW_ROUTE_IVF_PQ'),
    ('graph_fallback_ignores_overflow_error', 'common',
     '        && overflow == CUVS_HNSW_OVERFLOW_ACE && n >= 256', '        && n >= 256'),
    ('graph_fallback_on_any_exception', 'common',
     '        && what && strstr(what, CUVS_HNSW_GRAPH_KNN_REJECTED) != NULL;', '        && what;'),
    ('graph_fallback_ungated', 'wrapper',
     '            if (!cuvs_hnsw_graph_failure_to_ace(', '            if (0 && !cuvs_hnsw_graph_failure_to_ace('),
    ('graph_fallback_unrecorded', 'wrapper',
     '                | CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK;', '                | 0;'),
    ('graph_fallback_receipt_keeps_ivf_pq', 'wrapper',
     '            receipt.route = CUVS_HNSW_ROUTE_ACE;\n            receipt.internal_builder = 3;',
     '            receipt.internal_builder = 3;'),
    ('graph_fallback_flag_not_validated', 'policy',
     '            + !!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK)', '            + 0'),
    ('graph_fallback_notice_not_selected', 'preflight',
     '        return CUVS_HNSW_NOTICE_GRAPH_FALLBACK;', '        return 0;'),
    ('graph_fallback_without_notice', 'backend',
     '            if (notices & CUVS_HNSW_NOTICE_GRAPH_FALLBACK)\n                ereport(NOTICE',
     '            if (0)\n                ereport(NOTICE'),
    ('graph_fallback_exemption_widened', 'server',
     '        && route.decision.route == CUVS_HNSW_ROUTE_IVF_PQ && !route.decision.overflowed',
     '        && 1'),
    ('ace_sdk_host_budget_dropped', 'wrapper',
     'ace.max_host_memory_gb = (double)sdk_host_budget / (UINT64_C(1) << 30);', 'ace.max_host_memory_gb = 0;'),
    ('ace_sdk_gpu_budget_dropped', 'wrapper',
     'ace.max_gpu_memory_gb = (double)sdk_gpu_budget / (UINT64_C(1) << 30);', 'ace.max_gpu_memory_gb = 0;'),
    ('ace_without_memory_guard_marker', 'wrapper',
     '                    "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa")\n                || !hnsw_sdk_marker_verified("pgcuvs_hnsw_memory_guard_patch_id",\n                    "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa"))\n            {\n                snprintf(error, error_len, "ACE requires',
     '                    "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa"))\n            {\n                snprintf(error, error_len, "ACE requires'),
    ('third_ace_route_writer_in_daemon', 'server',
     '    params.auto_overflow = g_hnsw_auto_overflow;',
     '    params.auto_overflow = g_hnsw_auto_overflow;\n    if (0) wire.policy.route = CUVS_HNSW_ROUTE_ACE;'),
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
