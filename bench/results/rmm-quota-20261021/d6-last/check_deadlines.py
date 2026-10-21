"""Small offline boundary checks; no cloud or SSH commands."""
import json
from pathlib import Path
from deadline_budget import Budget, Exhausted

now = [0.0]
budget = Budget(lambda: now[0])
results = []


def denied(label, call):
    try:
        call()
    except Exhausted as exc:
        results.append({'case': label, 'denied': True, 'cause': str(exc)})
    else:
        raise AssertionError(label)


resume = budget.phase('resume')
assert resume.deadline == 180
now[0] = 179
assert resume.timeout(40) == 1
results.append({'case': 'resume_inventory_or_refresh_clipped', 'timeout': 1})
now[0] = 180
denied('resume_probe_after_deadline', lambda: resume.timeout(12))
now[0] = 1000
verify = budget.phase('verify')
assert verify.deadline == 1230
now[0] = 1229
assert verify.timeout(40) == 1
results.append({'case': 'next_work_preserves_570', 'timeout': 1})
now[0] = 1230
denied('no_work_when_cleanup_stop_reservation_reached', lambda: verify.timeout(1))
denied('no_new_work_phase_at_reservation', lambda: budget.phase('prepare'))
cleanup = budget.phase('cleanup')
assert cleanup.deadline == 1320
now[0] = 1319
assert cleanup.timeout(35) == 1
results.append({'case': 'cleanup_preserves_480_stop_seconds', 'timeout': 1})
now[0] = 1320
denied('cleanup_exhausted', lambda: cleanup.timeout(1))
stop = budget.phase('stop')
assert stop.deadline == 1800
now[0] = 1799
assert stop.timeout(40) == 1
results.append({'case': 'STOP_inventory_clipped_to_same_deadline', 'timeout': 1})
now[0] = 1800
denied('no_STOP_inventory_after_total_deadline', lambda: stop.timeout(40))
report = {'passed': len(results), 'cases': results, 'remote_calls': 0}
Path(__file__).with_name('deadline-checks.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
