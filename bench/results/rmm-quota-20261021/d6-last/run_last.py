"""Third and final resume, exact application pin, cumulative deadlines, normal STOP only."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
from deadline_budget import Budget, CAPS, RESERVE

HERE = Path(__file__).resolve().parent
REPO = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], cwd=HERE, text=True).strip())
sys.path.insert(0, str(HERE.parent/'transport-boundary'))
from ssh_readiness import accept_probe, probe_hostname

WORK = REPO/'.omc/rmm-cap-implementation/last/live'
HOST, VMID = 'pg-cuvs-index-accel-20261020', 'v6nc08sjs'
PIN = '0ff9f7c9fac4c20b8256dfc20ea3b0e604f7dd28'
ROOT = '/home/ubuntu/index-accel-validation'
OUT, TOOLS = ROOT+'/rmm-quota-last-20261021', ROOT+'/rmm-production-tools-last-20261021'
SSH = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', HOST]


def measurement_launch_argv(mode, arm, fixture=False):
    command = ['/opt/miniforge3/envs/cuvs_bench/bin/python', TOOLS+'/measurement_launch.py', mode, arm]
    if fixture:
        command += ['--fixture']
    return command


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    state = {'task': 'b40ecef03a934801', 'source_pin': PIN, 'vm_name': HOST, 'vm_id': VMID,
             'total_seconds': 1800, 'phase_caps': CAPS, 'future_reservation': RESERVE,
             'commands': [], 'phases': {}, 'first_error': None, 'cleanup_errors': [],
             'resumes_this_window': 0, 'prior_failed_window_resumes': 2,
             'native_passed': False, 'cleanup_passed': False, 'export_passed': False,
             'stop_confirmed': False, 'deadline_is_not_provider_or_billing_guarantee': True}
    budget = None
    resume_attempted = probe_ok = prepare_started = False

    def save():
        (WORK/'state.json').write_text(json.dumps(state, indent=2)+'\n')

    def fail(scope, exc, secondary=False):
        error = {'scope': scope, 'type': type(exc).__name__, 'message': str(exc),
                 'at_utc': utc(), 'traceback': traceback.format_exc()}
        if state['first_error'] is None:
            state['first_error'] = error
        if secondary:
            state['cleanup_errors'].append(error)
        save()

    def phase(name):
        p = budget.phase(name)
        state['phases'][name] = {'started_utc': utc(), 'started_elapsed': p.started-budget.started,
                                 'deadline_elapsed': p.deadline-budget.started}
        print(utc(), 'phase', name, flush=True)
        save()
        return p

    def end(p):
        state['phases'][p.name].update(seconds=budget.clock()-p.started, finished_utc=utc())
        save()

    def run(p, label, argv, cap, check=True, env=None):
        timeout = p.timeout(cap)  # admission happens before creating any subprocess
        started = time.monotonic()
        item = {'phase': p.name, 'label': label, 'started_utc': utc(),
                'timeout': timeout, 'elapsed_at_admission': started-budget.started}
        print(utc(), label, flush=True)
        try:
            with (WORK/(label+'.stdout')).open('w') as stdout, (WORK/(label+'.stderr')).open('w') as stderr:
                result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                        timeout=timeout, env=env, check=False)
            item['exit'] = result.returncode
            if check and result.returncode:
                tail = (WORK/(label+'.stderr')).read_text(errors='replace')[-2000:]
                raise RuntimeError(f'{label} exited {result.returncode}: {tail}')
            return result
        except subprocess.TimeoutExpired:
            item['exit'] = 'timeout'
            raise
        finally:
            item.update(seconds=time.monotonic()-started, finished_utc=utc())
            state['commands'].append(item)
            save()

    def inventory(p, label):
        run(p, label, ['brev', 'ls', '--json'], 40)
        raw = json.loads((WORK/(label+'.stdout')).read_text())
        rows = [w for w in raw['workspaces'] if w.get('id') == VMID]
        assert len(rows) == 1 and rows[0]['name'] == HOST
        state['last_inventory'] = {'observed_utc': utc(), 'workspace': rows[0]}
        save()
        return rows[0]

    def remote(p, label, command, cap):
        allowed = p.timeout(cap)
        if allowed < 6:
            raise RuntimeError(f'{label}: insufficient remaining time to bound remote process')
        # The remote process group expires before the local SSH timeout.
        argv = SSH+['flock','-n',ROOT+'/remote-owner.lock','timeout', '--signal=TERM', '--kill-after=3s', str(int(allowed-4))+'s']+command
        return run(p, label, argv, allowed)

    def stage(p, name, cap):
        return remote(p, name, ['bash', TOOLS+'/remote_followup.sh', PIN, name], cap)

    try:
        assert not (WORK/'resume-issued').exists(), 'one-resume marker exists; no fourth resume'
        condition = json.loads((HERE/'local-validation/precondition.json').read_text())
        assert condition['passed'] and condition['full_measurement_exit'] == 0
        assert condition['real_SQL_search_statements'] > 0 and condition['real_status_statements'] > 0
        for name, expected_sha in condition['input_sha256'].items():
            assert hashlib.sha256((HERE/name).read_bytes()).hexdigest() == expected_sha, name
        state['local_precondition_sha256'] = hashlib.sha256((HERE/'local-validation/precondition.json').read_bytes()).hexdigest()
        state['validation_head'] = subprocess.check_output(['git','rev-parse','HEAD'], cwd=REPO, text=True).strip()
        subprocess.run(['git','diff','--exit-code',PIN,'HEAD','--','.',':!bench/results/**'], cwd=REPO, check=True)
        assert not subprocess.check_output(['git','status','--porcelain'], cwd=REPO, text=True).strip()
        state['application_tree_equals_pin'] = True
        state['runner_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        state['window_started_utc'] = utc()
        budget = Budget()
        p = phase('resume')
        before = inventory(p, 'before')
        assert before['status'] == 'STOPPED', before
        with (WORK/'resume-issued').open('x') as marker:
            marker.write(utc()+'\n')
        resume_attempted = True
        state['resumes_this_window'] = 1
        run(p, 'vm-start', ['brev','start',HOST,'--detached'], 60)
        attempt = 0
        while True:
            row = inventory(p, 'boot-%02d'%attempt)
            if row['status'] == 'RUNNING' and row['shell_status'] == 'READY':
                break
            p.pause(5)
            attempt += 1
        run(p, 'ssh-refresh', ['brev','refresh'], 40)
        # Exactly one actual SSH probe; failure goes straight to normal STOP.
        probe = probe_hostname(SSH+['hostname'], 'brev-v6nc08sjs',
                               WORK/'probe.stdout', WORK/'probe.stderr', p.timeout(12))
        state['actual_ssh_probe'] = probe
        save()
        if not accept_probe(probe):
            raise RuntimeError(probe['error'])
        probe_ok = True
        run(p, 'tools-mkdir', SSH+['mkdir','-p',TOOLS], 15)
        files = ['remote_followup.sh','graph_check.py','status_and_binding.py',
                 'measurement_entry.py','measurement_launch.py','check_smoke.py']
        run(p, 'tools-copy', ['scp','-o','BatchMode=yes','-o','ConnectTimeout=8']+
            [str(HERE/name) for name in files]+[HOST+':'+TOOLS+'/'], 25)
        remote(p, 'connect-preflight', measurement_launch_argv('connect','zero'), 25)
        end(p)

        p = phase('prepare')
        prepare_started = True
        stage(p, 'prepare', 260)
        end(p)

        p = phase('verify')
        for arm, initial, percent in [('zero',0,0),('default',512,50)]:
            stage(p, arm, 75)
            env = os.environ.copy(); env['VM_SSH_HOST'] = HOST
            run(p, 'preflight-'+arm, ['bash',str(REPO/'infra/scripts/gpu-preflight.sh'),'index_accel_smoke'],25,env=env)
            remote(p, 'status-'+arm, ['/opt/miniforge3/envs/cuvs_bench/bin/python',TOOLS+'/status_and_binding.py',
                                     '/home/ubuntu/pg_cuvs',OUT+'/status-'+arm+'.json',str(initial),str(percent)],25)
            remote(p, 'measure-'+arm, measurement_launch_argv('measure',arm), 120)
            remote(p, 'quality-'+arm, ['python3',TOOLS+'/check_smoke.py',OUT+'/smoke-'+arm,arm],10)
        stage(p, 'reject', 45)
        stage(p, 'admission', 75)
        remote(p, 'status-admission', ['/opt/miniforge3/envs/cuvs_bench/bin/python',TOOLS+'/status_and_binding.py',
                                      '/home/ubuntu/pg_cuvs',OUT+'/status-admission.json','64','1'],25)
        state['native_passed'] = True
        end(p)
    except Exception as exc:
        fail('native', exc)
    finally:
        if resume_attempted:
            try:
                p = phase('cleanup')
                if probe_ok and prepare_started:
                    try:
                        stage(p, 'cleanup', 55)
                        state['cleanup_passed'] = True
                    except Exception as exc:
                        fail('remote_cleanup',exc,True)
                    try:
                        (WORK/'native-output').mkdir(exist_ok=True)
                        run(p, 'evidence-copy', ['rsync','-a','--timeout=30','-e',
                            'ssh -T -o BatchMode=yes -o ConnectTimeout=8',HOST+':'+OUT+'/',str(WORK/'native-output')+'/'],35)
                        state['export_passed'] = True
                    except Exception as exc:
                        fail('export',exc,True)
                else:
                    state['cleanup_skipped'] = 'probe/prepare did not pass; no additional remote work'
                end(p)
            except Exception as exc:
                fail('cleanup_budget',exc,True)
            try:
                p = phase('stop')
                state['stop_request_started_utc'] = utc()
                result = run(p, 'vm-stop', ['brev','stop',HOST],45,check=False)
                state['stop_request_accepted'] = result.returncode == 0
                state['stop_request_completed_utc'] = utc()
                if result.returncode:
                    fail('stop_request',RuntimeError('normal STOP CLI exited '+str(result.returncode)),True)
                poll = 0
                while True:
                    row = inventory(p,'stop-observation-%02d'%poll)
                    if row['status'] == 'STOPPED':
                        state['stop_confirmed'] = True
                        state['STOPPED_observed_utc'] = state['last_inventory']['observed_utc']
                        break
                    p.pause(5)
                    poll += 1
                end(p)
            except Exception as exc:
                fail('stop_confirmation',exc,True)
        if budget:
            state['elapsed_seconds'] = time.monotonic()-budget.started
            state['within_total'] = state['elapsed_seconds'] <= 1800
            state['compute_reference_usd'] = state['elapsed_seconds']/3600*1.74
            state['cost_note'] = 'window observation at stored rate; not invoice or physical poweroff measurement'
        state['finished_utc'] = utc()
        state['succeeded'] = bool(state['native_passed'] and state['cleanup_passed'] and state['export_passed']
                                  and state['stop_confirmed'] and state.get('within_total') and not state['first_error'])
        save()
        print(json.dumps(state,indent=2),flush=True)
    return 0 if state['succeeded'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
