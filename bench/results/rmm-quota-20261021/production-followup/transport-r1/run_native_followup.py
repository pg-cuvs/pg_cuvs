import datetime
import json
import os
from pathlib import Path
import subprocess
import time
import traceback

LOCAL = Path(__file__).resolve().parent
REPO = LOCAL.parent.parent
HOST = 'pg-cuvs-index-accel-20261020'
PIN = '0ff9f7c9fac4c20b8256dfc20ea3b0e604f7dd28'
ROOT = '/home/ubuntu/index-accel-validation'
OUT = ROOT + '/rmm-quota-production-20261021'
TOOLS = ROOT + '/rmm-production-tools'
SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', HOST]
began = None
deadline = None
record = {'source_pin': PIN, 'vm': HOST, 'vm_id': 'v6nc08sjs',
          'budget_seconds': 900, 'price_usd_per_hour_reference': 1.74,
          'cost_is_estimate_not_invoice': True, 'phases': {}}

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def run(label, argv, limit=60, final=False, check=True, **kwargs):
    limit = min(limit, max(1, (deadline - time.monotonic()) if deadline and not final else limit))
    print(now(), label, flush=True)
    started = time.monotonic()
    path = LOCAL / (label + '.log')
    try:
        with path.open('w') as stream:
            r = subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT, timeout=limit, **kwargs)
        record['phases'][label] = {'exit': r.returncode, 'seconds': time.monotonic()-started}
        if check and r.returncode:
            print(path.read_text(errors='replace')[-3000:], flush=True)
            raise RuntimeError(label + ' failed: ' + str(r.returncode))
        return r
    except subprocess.TimeoutExpired:
        record['phases'][label] = {'exit': 'timeout', 'seconds': time.monotonic()-started}
        raise
    finally:
        (LOCAL/'native-state.json').write_text(json.dumps(record, indent=2)+'\n')

def stage(label, limit=120, final=False):
    return run(label, SSH + ['bash', TOOLS+'/remote_followup.sh', PIN, label], limit, final)

def inventory():
    r = subprocess.run(['brev','ls','--json'], capture_output=True, text=True, timeout=40)
    r.check_returncode()
    rows = [x for x in json.loads(r.stdout)['workspaces'] if x.get('id') == 'v6nc08sjs']
    assert len(rows) == 1 and rows[0]['name'] == HOST
    return rows[0]

try:
    assert subprocess.check_output(['git','rev-parse','HEAD'], cwd=REPO, text=True).strip() == PIN
    assert not subprocess.check_output(['git','status','--porcelain'], cwd=REPO, text=True).strip()
    before = inventory()
    assert before['status'] == 'STOPPED', before
    record['before'] = before
    record['started_at_utc'] = now()
    began = time.monotonic()
    deadline = began + 720  # reserve three minutes for export, cleanup and STOP
    run('vm-start', ['brev','start',HOST,'--detached'], 60)
    run('ssh-refresh', ['brev','refresh'], 40)
    ready = False
    for attempt in range(18):
        r = run('ready-%02d'%attempt, SSH+['hostname'], 12, check=False)
        if r.returncode == 0:
            assert (LOCAL/('ready-%02d.log'%attempt)).read_text().strip() == 'brev-v6nc08sjs'
            ready = True
            break
        time.sleep(5)
    assert ready, 'SSH did not become ready within the bounded boot window'
    run('tools-mkdir', SSH+['mkdir','-p',TOOLS], 15)
    run('tools-copy', ['scp','-o','BatchMode=yes','-o','ConnectTimeout=8',
                      str(LOCAL/'remote_followup.sh'), str(LOCAL/'graph_check.py'),
                      str(LOCAL/'status_and_binding.py'), str(LOCAL/'feature_gpu_check_smoke_frozen.py'), HOST+':'+TOOLS+'/'], 20)
    stage('prepare', 260)
    for arm, initial, percent in [('zero',0,0), ('default',512,50)]:
        if deadline - time.monotonic() < 100:
            raise RuntimeError('insufficient budget for next arm; no scope expansion')
        stage(arm, 75)
        env = os.environ.copy(); env['VM_SSH_HOST'] = HOST
        run('preflight-'+arm, ['bash',str(REPO/'infra/scripts/gpu-preflight.sh'),'index_accel_smoke'], 25, env=env)
        run('status-'+arm, SSH+['/opt/miniforge3/envs/cuvs_bench/bin/python',TOOLS+'/status_and_binding.py',
                              '/home/ubuntu/pg_cuvs',OUT+'/status-'+arm+'.json',str(initial),str(percent)], 25)
        run('measure-'+arm, SSH+['/opt/miniforge3/envs/cuvs_bench/bin/python',TOOLS+'/feature_gpu_check_smoke_frozen.py',
                               'measure','--repo','/home/ubuntu/pg_cuvs','--owner-file',ROOT+'/owner.json',
                               '--expected-commit',PIN,'--data-dir','/home/ubuntu/data','--out',OUT+'/smoke-'+arm,
                               '--baseline',ROOT+'/results/baseline_control1','--noise-reference',ROOT+'/results/baseline-noise.json',
                               '--queries','10000','--points','64,128','--expect-rmm-mb',str(initial)], 120)
    stage('reject', 45)
    stage('admission', 75)
    run('status-admission', SSH+['/opt/miniforge3/envs/cuvs_bench/bin/python',TOOLS+'/status_and_binding.py',
                               '/home/ubuntu/pg_cuvs',OUT+'/status-admission.json','64','1'], 25)
    record['native_passed'] = True
except Exception as exc:
    record['native_passed'] = False
    record['failure'] = str(exc)
    record['traceback'] = traceback.format_exc()
    print(record['traceback'], flush=True)
finally:
    if began is not None:
        try:
            stage('cleanup', 55, final=True)
            record['cleanup_passed'] = True
        except Exception as exc:
            record['cleanup_passed'] = False
            record['cleanup_failure'] = str(exc)
        try:
            (LOCAL/'native-output').mkdir(exist_ok=True)
            run('evidence-copy', ['rsync','-a','--timeout=30','-e','ssh -o BatchMode=yes -o ConnectTimeout=8',
                                  HOST+':'+OUT+'/',str(LOCAL/'native-output')+'/'], 35, final=True)
        except Exception as exc:
            record['export_failure'] = str(exc)
        try:
            run('vm-stop', ['brev','stop',HOST], 45, final=True)
            for attempt in range(8):
                after = inventory()
                record['after'] = after
                if after['status'] == 'STOPPED':
                    break
                time.sleep(3)
            record['stop_confirmed'] = record['after']['status'] == 'STOPPED'
        except Exception as exc:
            record['stop_failure'] = str(exc)
        record['finished_at_utc'] = now()
        record['observed_running_seconds_upper_bound'] = time.monotonic() - began
        record['estimated_compute_usd'] = record['observed_running_seconds_upper_bound']/3600*1.74
        record['within_900s'] = record['observed_running_seconds_upper_bound'] <= 900
    (LOCAL/'native-state.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record, indent=2), flush=True)

raise SystemExit(0 if record.get('native_passed') and record.get('cleanup_passed') and
                 record.get('stop_confirmed') and record.get('within_900s') and
                 not record.get('export_failure') else 1)
