"""Run the real upper argv builder -> launcher -> complete entry in local PG."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import run_last
import measurement_launch

HERE = Path(__file__).resolve().parent
NAME = 'pgcuvs-rmm-last-fixture-20261021'
TOOLS = measurement_launch.TOOLS
OUTPUT = HERE/'local-validation'
OUTPUT.mkdir(exist_ok=True)


def execute(label, argv):
    result = subprocess.run(argv,capture_output=True,text=True)
    (OUTPUT/(label+'.stdout')).write_text(result.stdout)
    (OUTPUT/(label+'.stderr')).write_text(result.stderr)
    if result.returncode:
        print(result.stdout[-1500:]);print(result.stderr[-3000:])
        raise RuntimeError(f'{label} exited {result.returncode}')
    return result


def main():
    execute('tools-mkdir',['docker','exec',NAME,'mkdir','-p',TOOLS])
    for name in ['measurement_launch.py','measurement_entry.py','local_fixture_data.py']:
        execute('copy-'+name,['docker','cp',str(HERE/name),NAME+':'+TOOLS+'/'+name])
    execute('fixture-owner',['docker','exec',NAME,'chown','-R','postgres:postgres',measurement_launch.ROOT])
    execute('fixture-data',['docker','exec','--user','postgres','--workdir',measurement_launch.CWD,
                            NAME,measurement_launch.PYTHON,TOOLS+'/local_fixture_data.py'])
    upper_commands = {}
    for mode in ['connect','prepare','measure']:
        # This is the exact callable used by the live upper runner.
        command = run_last.measurement_launch_argv(mode,'zero',fixture=True)
        upper_commands[mode] = command
        execute(mode,['docker','exec','--user','postgres','--workdir',measurement_launch.CWD,NAME]+command)
    execute('copy-results',['docker','cp',NAME+':'+measurement_launch.OUT,str(OUTPUT/'entry-results')])
    execute('copy-SQL-log',['docker','cp',NAME+':/tmp/postgres.log',str(OUTPUT/'postgres.log')])
    log = (OUTPUT/'postgres.log').read_text()
    searches = [line for line in log.splitlines() if 'SELECT id, embedding <->' in line and 'EXPLAIN' not in line]
    shows = [line for line in log.splitlines() if 'SHOW cuvs.rmm_pool_mb' in line]
    assert len(searches) >= 16 and len(shows) >= 3
    output = OUTPUT/'entry-results'
    metrics = json.loads((output/'smoke-zero/metrics.json').read_text())
    assert len(metrics)==2 and all(p['observed_gpu_queries']==8 and p['observed_gpu_errors']==0 for p in metrics)
    connect = json.loads((output/'connect-zero/connect.json').read_text())
    assert connect['select1']==1 and connect['rmm_pool_mb']=='0'
    assert connect['PGHOST']=='/var/run/postgresql' and connect['cwd']==measurement_launch.CWD
    meta = json.loads((output/'smoke-zero/metadata.json').read_text())
    assert meta['commit']==run_last.PIN and meta['n']==256 and meta['queries']==8
    inputs = ['run_last.py','measurement_launch.py','measurement_entry.py','deadline_budget.py',
              'remote_followup.sh','graph_check.py','status_and_binding.py','check_smoke.py',
              'run_local_fixture.py','local_fixture_data.py']
    hashes = {name:hashlib.sha256((HERE/name).read_bytes()).hexdigest() for name in inputs}
    assert meta['harness_file_sha256']==hashes['measurement_entry.py']==connect['entry_sha256']
    for mode in ['connect','prepare','measure']:
        launch = json.loads((output/('launch-'+mode+'-zero.json')).read_text())
        assert launch['exit']==0 and launch['launcher_sha256']==hashes['measurement_launch.py']
        assert launch['entry_sha256']==hashes['measurement_entry.py']
        assert launch['argv']==measurement_launch.measurement_argv(mode,'zero',fixture=True)
    record = {'passed':True,'full_measurement_exit':0,'real_SQL_search_statements':len(searches),
              'real_status_statements':len(shows),'input_sha256':hashes,'upper_runner_callable':'run_last.measurement_launch_argv',
              'upper_commands':upper_commands,'actual_connection':connect,
              'native_measure_argv':{arm:measurement_launch.measurement_argv('measure',arm) for arm in ['zero','default']},
              'fixture_measure_argv':measurement_launch.measurement_argv('measure','zero',True),
              'same_paths_env_cwd':True,'source_pin':meta['commit'],'PGHOST':'/var/run/postgresql','cwd':measurement_launch.CWD,
              'fixture_difference':{'rows':256,'queries_per_point':8,'points':[64,128],'CPU_shim':True,
                  'hardware_GPU':False,'native_control_comparison_omitted_for_fixture':True,
                  'protocol_gpu_counters_are_emulated_labels_not_GPU_evidence':True},
              'mocked_connections_or_SQL_or_status':False,'early_termination_of_measure_mode':False,
              'GPU_quality_evidence':False}
    (OUTPUT/'precondition.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'passed':True,'search_statements':len(searches),'status_statements':len(shows),
                      'metrics':metrics,'connection':connect},indent=2))


if __name__ == '__main__':
    main()
