"""Offline estimates from frozen job timestamps; does not authorize a run."""
from datetime import datetime, timedelta
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PREVIOUS = ROOT.parent/'production-followup'
HISTORY = ROOT.parent.parent/'index-accel-20261021/logs'


def utc(value):
    return datetime.fromisoformat(value.replace('Z', '+00:00'))


def main():
    previous = json.loads((PREVIOUS/'outcome.json').read_text())
    first = json.loads((PREVIOUS/'transport-r1/native-state.json').read_text())
    lines = (PREVIOUS/'transport-r1/native-runner.log').read_text().splitlines()
    ready_started = next(utc(line.split()[0]) for line in lines if line.endswith(' ready-12'))
    ready_at = ready_started + timedelta(seconds=first['phases']['ready-12']['seconds'])
    resumed_at = utc(first['started_at_utc'])
    history = {}
    for name in ['deploy_guc', 'deploy_rmm0_linkfix', 'deploy_rmm_sweep',
                 'rmm_zero2', 'rmm_zero3', 'rmm1024_smoke', 'preserve_graph']:
        assert (HISTORY/(name+'.exit')).read_text().strip() == '0'
        start = utc((HISTORY/(name+'.started')).read_text().strip())
        end = utc((HISTORY/(name+'.completed')).read_text().strip())
        history[name] = {'seconds': (end-start).total_seconds(), 'exit': 0,
                         'evidence': '../../index-accel-20261021/logs/'+name+'.{started,completed,exit}',
                         'scope': 'prior snapshot, same L40S lane; whole job, not isolated compiler/kernel time'}
    phases = [
        {'phase': 'resume_and_readiness', 'estimate_seconds': 90, 'proposed_cap_seconds': 180,
         'basis': 'observed start-to-valid-hostname 80.45s N=1, including SSH refresh retries'},
        {'phase': 'prepare_build_codeploy_and_graph', 'estimate_seconds': 90, 'proposed_cap_seconds': 260,
         'basis': 'prior successful deploy jobs37-39s; graph copy+hash9s; new pin/binding work unmeasured'},
        {'phase': 'two_smokes_status_and_quota', 'estimate_seconds': 210, 'proposed_cap_seconds': 635,
         'basis': 'prior two-point smoke jobs50-54s per arm; extra status/preflight/quota estimated; cap=2*(75+25+25+120)+(45+75+25)'},
        {'phase': 'cleanup_and_export', 'estimate_seconds': 45, 'proposed_cap_seconds': 90,
         'basis': 'new cleanup and graph recheck/export unmeasured; existing requested limits55+35'},
        {'phase': 'stop_request_through_STOPPED_observation', 'estimate_seconds': 420, 'proposed_cap_seconds': 480,
         'basis': 'observed request-to-STOPPED383.676s N=1, includes request CLI and polling; no second CLI allowance added'},
    ]
    point = sum(p['estimate_seconds'] for p in phases)
    caps = sum(p['proposed_cap_seconds'] for p in phases)
    proposed = 1800
    remaining = 900-previous['elapsed_to_STOPPED_seconds']
    plan = {
        'source_pin': previous['source_pin'], 'local_only': True, 'new_VM_resumes': 0,
        'last_STOPPED_observation': previous['STOPPED_observed_at_utc'],
        'observed': {'start_to_valid_hostname_seconds': (ready_at-resumed_at).total_seconds(),
                     'stop_request_to_STOPPED_observation_seconds': previous['observed_stop_transition_seconds'],
                     'resume_to_STOPPED_observation_seconds': previous['elapsed_to_STOPPED_seconds'],
                     'stop_observation_is_not_physical_poweroff_or_billing_measurement': True,
                     'historical_successful_jobs': history},
        'phases': phases, 'point_estimate_seconds': point, 'proposed_phase_caps_seconds': caps,
        'proposed_fresh_unit_seconds': proposed, 'fresh_unit_slack_after_caps_seconds': proposed-caps,
        'original_900s_remaining_seconds': remaining,
        'remaining_after_480s_stop_reserve_seconds': remaining-480,
        'old_900s_deficit_against_phase_caps_seconds': caps-900,
        'admission': {'remaining_original_budget': remaining >= caps,
                      'fresh_900_seconds': 900 >= caps,
                      'proposed_1800_arithmetic_fit_only': proposed >= caps},
        'price_reference_usd_per_hour': previous['displayed_price_usd_per_hour'],
        'proposed_30min_compute_reference_usd': proposed/3600*previous['displayed_price_usd_per_hour'],
        'cost_note': 'stored catalog rate; scenario estimate, not an invoice or new authorization',
        'unmeasured': ['CUDA compile/co-deploy on exact0ff9f7c',
                       'new positive per-device status, mapped-binary binding and quota checks',
                       'new cleanup/export duration', 'stop-latency distribution and tail bound',
                       'actual billable duration and invoice'],
        'executor_changes_required_before_any_new_live_unit': [
            'Allocate a fresh explicit total budget; current900s remains insufficient.',
            'Enforce aggregate resume180s and stop-confirmation480s deadlines; do not rely on20 unbounded40s inventories.',
            'Reserve cleanup90s+STOP480s before starting next work phase; no work after that reservation is consumed.',
            'Refresh alias after actual readiness within resume phase, preserving transport causes.',
            'Retain exact source, graph, query count, quality criteria and failure evidence.'
        ],
        'authorization': 'proposal only; current local task excludes resume; no new live run authorized here',
    }
    assert point == 855 and caps == 1645 and proposed-caps == 155
    assert not plan['admission']['remaining_original_budget']
    assert not plan['admission']['fresh_900_seconds']
    assert plan['admission']['proposed_1800_arithmetic_fit_only']
    (ROOT/'time-plan.json').write_text(json.dumps(plan, indent=2)+'\n')
    print(json.dumps(plan, indent=2))


if __name__ == '__main__':
    main()
