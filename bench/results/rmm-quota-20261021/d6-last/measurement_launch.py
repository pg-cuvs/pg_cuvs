"""Shared final launcher: real PgEngine entry, explicit socket env and cwd."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = '/home/ubuntu/index-accel-validation'
TOOLS = ROOT+'/rmm-production-tools-last-20261021'
OUT = ROOT+'/rmm-quota-last-20261021'
REPO = '/home/ubuntu/pg_cuvs'
PYTHON = '/opt/miniforge3/envs/cuvs_bench/bin/python'
PIN = '0ff9f7c9fac4c20b8256dfc20ea3b0e604f7dd28'
CWD = '/home/ubuntu'


def measurement_argv(mode, arm, fixture=False):
    assert mode in ('connect','prepare','measure') and arm in ('zero','default')
    assert mode != 'prepare' or fixture, 'native retained graph must not be rebuilt'
    assert not fixture or arm == 'zero', 'CPU shim does not validate a GPU pool'
    output = ('connect-'+arm if mode == 'connect' else
              'prepare-fixture' if mode == 'prepare' else 'smoke-'+arm)
    argv = ['env','PGHOST=/var/run/postgresql',PYTHON,TOOLS+'/measurement_entry.py',mode,
            '--repo',REPO,'--owner-file',ROOT+'/owner.json','--expected-commit',PIN,
            '--dbname','index_accel_smoke','--data-dir','/home/ubuntu/data',
            '--index-dir','/tmp/cuvs_indexes','--out',OUT+'/'+output,
            '--n',str(256 if fixture else 1000000),'--queries',str(8 if fixture else 10000),
            '--points','64,128','--expect-rmm-mb','0' if arm == 'zero' else '512']
    if not fixture:
        argv += ['--baseline',ROOT+'/results/baseline_control1',
                 '--noise-reference',ROOT+'/results/baseline-noise.json']
        if arm == 'zero':
            argv += ['--enforce-noise-floor']
    return argv


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode',choices=['connect','prepare','measure'])
    parser.add_argument('arm',choices=['zero','default'])
    parser.add_argument('--fixture',action='store_true')
    args = parser.parse_args()
    command = measurement_argv(args.mode,args.arm,args.fixture)
    record = {'argv':command,'cwd':CWD,'explicit_env':{'PGHOST':'/var/run/postgresql'},
              'fixture':args.fixture,'source_pin':PIN,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'entry_sha256':hashlib.sha256(Path(TOOLS+'/measurement_entry.py').read_bytes()).hexdigest(),
              'launcher_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    Path(OUT).mkdir(parents=True,exist_ok=True)
    target = Path(OUT)/('launch-'+args.mode+'-'+args.arm+'.json')
    target.write_text(json.dumps(record,indent=2)+'\n')
    print('LAUNCH '+json.dumps(record),flush=True)
    start = time.monotonic()
    result = subprocess.run(command,cwd=CWD,check=False)
    record.update(exit=result.returncode,seconds=time.monotonic()-start)
    target.write_text(json.dumps(record,indent=2)+'\n')
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
