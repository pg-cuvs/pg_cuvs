"""CK-13 [r][s][t] input tables for q4_prespool_judge, plus judge mutants.

Run: python3 test/unit/test_q4_prespool_judge.py   (exit 0 = all rows and all
mutants detected). Each mutant is one wrong runner the plan says to reject.
"""
import copy
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = (HERE / 'q4_prespool_judge.py').read_text()


def load(source):
    spec = importlib.util.spec_from_loader('judge', loader=None)
    mod = importlib.util.module_from_spec(spec)
    exec(compile(source, 'q4_prespool_judge.py', 'exec'), mod.__dict__)
    return mod


T = 'pg_cuvs: build_algo=auto exceeded the resident budget (need 1660 MiB, budget 1536 MiB); building with ACE on disk'
C = 'pg_cuvs: auto selected ACE-disk (128 partitions): resident IVF-PQ estimate exceeds budget'
BASE6 = dict(reltuples=1e6, spool_start_observed=True, first_spool_sample_t=10.0,
             notices=[(2.0, T), (140.0, C)], capture_complete=True,
             receipt_requested_algo='auto', receipt_route='ace_disk', route_log_route='ace_disk',
             recall_at_floor=True)


def v(base, **kw):
    o = copy.deepcopy(base); o.update(kw); return o


ROWS_R = [  # [r] Q4-6
    ('pass', v(BASE6), 'PASS'),
    ('notice_after_spool', v(BASE6, notices=[(11.0, T), (140.0, C)]), 'FAIL'),
    ('notice_missing', v(BASE6, notices=[(140.0, C)]), 'FAIL'),
    ('completion_missing', v(BASE6, notices=[(2.0, T)]), 'FAIL'),
    ('completion_twice', v(BASE6, notices=[(2.0, T), (100.0, C), (140.0, C)]), 'FAIL'),
    ('receipt_vs_route_log', v(BASE6, route_log_route='resident_ivf_pq'), 'FAIL'),
    ('receipt_not_ace', v(BASE6, receipt_route='resident_ivf_pq', route_log_route='resident_ivf_pq'), 'FAIL'),
    ('recall_below_floor', v(BASE6, recall_at_floor=False), 'FAIL'),
    ('reltuples_zero', v(BASE6, reltuples=0.0), 'INDETERMINATE'),
    ('reltuples_zero_even_if_notice_late', v(BASE6, reltuples=0.0, notices=[(11.0, T)]), 'INDETERMINATE'),
    ('sampler_missed_spool_start', v(BASE6, spool_start_observed=False), 'INDETERMINATE'),
    ('capture_incomplete_notice_missing', v(BASE6, capture_complete=False, notices=[]), 'INDETERMINATE'),
    ('capture_incomplete_receipt_mismatch_still_fails',
     v(BASE6, capture_complete=False, notices=[], route_log_route='resident_ivf_pq'), 'FAIL'),
]
BASE4 = dict(base_state='FAIL', refusal_t=5.0, spool_sample_ts=[], sampling_covers_refusal=True)
ROWS_S = [  # [s] Q4-4 pre-spool disk check
    ('pass', v(BASE4), 'PASS'),
    ('spool_before_refusal', v(BASE4, spool_sample_ts=[3.0, 4.0]), 'FAIL'),
    ('spool_after_refusal_ok', v(BASE4, spool_sample_ts=[6.0]), 'PASS'),
    ('no_refusal', v(BASE4, refusal_t=None), 'FAIL'),
    ('sampler_missed_refusal', v(BASE4, sampling_covers_refusal=False, spool_sample_ts=[3.0]), 'INDETERMINATE'),
    ('base_q4_4_indeterminate', v(BASE4, base_state='INDETERMINATE'), 'INDETERMINATE'),
]
BASE3 = dict(reltuples=1e7, analyze_logged=True, ar0_refused=True, corpus_received_before_refusal=False)
ROWS_T = [  # [t] QE-3 reltuples AR0 guard
    ('pass', v(BASE3), 'PASS'),
    ('corpus_received_first', v(BASE3, corpus_received_before_refusal=True), 'FAIL'),
    ('no_ar0', v(BASE3, ar0_refused=False), 'FAIL'),
    ('reltuples_zero', v(BASE3, reltuples=0.0, corpus_received_before_refusal=True), 'INDETERMINATE'),
    ('no_analyze_record', v(BASE3, analyze_logged=False), 'INDETERMINATE'),
]


def run(mod):
    bad = []
    for fn, rows in ((mod.judge_q4_6, ROWS_R), (mod.judge_q4_4_prespool, ROWS_S), (mod.judge_qe3_reltuples, ROWS_T)):
        for name, obs, want in rows:
            got = fn(obs)
            if got != want:
                bad.append((fn.__name__, name, want, got))
    return bad


MUTANTS = [
    ('late_notice_passes', "transitions[0] < obs['first_spool_sample_t']", "transitions[0] < 1e18"),
    ('completion_not_required', "and len(completions) == 1 and completions[0] > transitions[0])", ")"),
    ('route_log_not_compared', "or obs['receipt_route'] != obs['route_log_route']):", "):"),
    ('reltuples_zero_fails', "if not obs['reltuples'] > 0 or not obs['spool_start_observed']", "if not obs['spool_start_observed']"),
    ('capture_failure_is_fail', "return FAIL if obs['capture_complete'] else INDET", "return FAIL"),
    ('missed_sampling_is_fail', "if obs['base_state'] == INDET or not obs['sampling_covers_refusal']:", "if obs['base_state'] == INDET:"),
    ('spool_before_refusal_passes', "if any(t <= obs['refusal_t'] for t in obs['spool_sample_ts']):", "if False:"),
    ('corpus_after_ar0_passes', "if not obs['ar0_refused'] or obs['corpus_received_before_refusal']:", "if not obs['ar0_refused']:"),
    ('qe3_no_stats_fails', "if not obs['reltuples'] > 0 or not obs['analyze_logged']:\n        return INDET", "if False:\n        return INDET"),
]


def main():
    bad = run(load(SRC))
    assert not bad, ('BASELINE', bad)
    detected = []
    for name, old, new in MUTANTS:
        assert SRC.count(old) == 1, ('ANCHOR', name, SRC.count(old))
        if run(load(SRC.replace(old, new, 1))):
            detected.append(name)
        else:
            raise AssertionError('JUDGE_MUTANT_SURVIVED:' + name)
    rows = len(ROWS_R) + len(ROWS_S) + len(ROWS_T)
    print('PASS CK-13 [r][s][t]: %d input rows, %d/%d judge mutants detected (%s)'
          % (rows, len(detected), len(MUTANTS), ', '.join(detected)))


if __name__ == '__main__':
    sys.exit(main())
