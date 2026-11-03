"""Pure judges for the I3-C Q items (v6 r18 CK-13 [r][s][t]). Stdlib only.

Offered to the Q runner (main owns q_runner.py); no SQL, SSH or VM access.
Times are seconds on one monotonic clock shared by the psql NOTICE capture and
the 1 Hz workdir sampler. Returns 'PASS', 'FAIL' or 'INDETERMINATE'.
"""

PASS, FAIL, INDET = 'PASS', 'FAIL', 'INDETERMINATE'
TRANSITION = 'pg_cuvs: build_algo=auto exceeded the resident budget'
COMPLETION = 'auto selected ACE-disk'


def judge_q4_6(obs):
    """Q4-6 pre-scan route decision (PN-1).

    obs keys: reltuples (float), spool_start_observed (bool: the sampler saw
    the empty -> first-spool-file transition), first_spool_sample_t (float or
    None), notices [(t, text)], capture_complete (bool: the known NOTICE
    arrived), receipt_requested_algo, receipt_route, route_log_route,
    recall_at_floor (bool).
    """
    if not obs['reltuples'] > 0 or not obs['spool_start_observed'] or obs['first_spool_sample_t'] is None:
        return INDET  # statistics premise or sampling premise not met
    if (obs['receipt_requested_algo'] != 'auto' or obs['receipt_route'] != 'ace_disk'
            or obs['receipt_route'] != obs['route_log_route']):
        return FAIL
    if not obs['recall_at_floor']:
        return FAIL
    transitions = [t for t, text in obs['notices'] if TRANSITION in text]
    completions = [t for t, text in obs['notices'] if COMPLETION in text]
    notice_ok = (len(transitions) >= 1 and transitions[0] < obs['first_spool_sample_t']
                 and len(completions) == 1 and completions[0] > transitions[0])
    if notice_ok:
        return PASS
    return FAIL if obs['capture_complete'] else INDET  # absence is not proven


def judge_q4_4_prespool(obs):
    """Q4-4 disk refusal precedes the spool (JI 'Q4-4 pre-spool disk check').

    obs keys: base_state (Q4-4 verdict), refusal_t (float or None),
    spool_sample_ts [float], sampling_covers_refusal (bool).
    """
    if obs['base_state'] == INDET or not obs['sampling_covers_refusal']:
        return INDET
    if obs['refusal_t'] is None:
        return FAIL
    if any(t <= obs['refusal_t'] for t in obs['spool_sample_ts']):
        return FAIL  # the precheck ran after spooling started
    return PASS


def judge_qe3_reltuples(obs):
    """QE-3 reltuples AR0 guard (JI 'QE-3 reltuples AR0 guard').

    obs keys: reltuples (float), analyze_logged (bool), ar0_refused (bool),
    corpus_received_before_refusal (bool, daemon journal).
    JI-7's AR0 SQLSTATE judgement is independent and evaluated elsewhere.
    """
    if not obs['reltuples'] > 0 or not obs['analyze_logged']:
        return INDET
    if not obs['ar0_refused'] or obs['corpus_received_before_refusal']:
        return FAIL
    return PASS
