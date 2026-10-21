"""Offline check of the repaired runner's actual measurement env prefix."""
import ast
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
repo = Path(subprocess.check_output(['git','rev-parse','--show-toplevel'],cwd=here,text=True).strip())
relative = str((here/'run_d6.py').relative_to(repo))
old = subprocess.check_output(['git','show','1022996:'+relative],cwd=repo)
current = (here/'run_d6.py').read_bytes()
before, after = ast.parse(old), ast.parse(current)
args = next(n.value for n in ast.walk(after) if isinstance(n,ast.Assign)
            and any(isinstance(t,ast.Name) and t.id=='args' for t in n.targets))
prefix = [ast.literal_eval(n) for n in args.elts[:2]]
assert prefix == ['env','PGHOST=/var/run/postgresql']
del args.elts[:2]
assert ast.dump(before) == ast.dump(after), 'any change besides PGHOST prefix is unexpected'
parent = os.environ.copy(); parent['PGHOST'] = '/tmp/wrong-parent-socket'
child = subprocess.run(prefix+[sys.executable,'-c','import os; print(os.environ["PGHOST"])'],
                       env=parent,capture_output=True,text=True,check=True)
assert child.stdout.strip() == '/var/run/postgresql'
report = {'passed':True,'executed_runner_commit':'1022996',
          'executed_runner_sha256':hashlib.sha256(old).hexdigest(),
          'repaired_runner_sha256':hashlib.sha256(current).hexdigest(),
          'actual_runner_env_prefix':prefix,'overrides_wrong_parent_PGHOST':True,
          'all_other_runner_AST_including_quality_gates_unchanged':True,
          'remote_calls':0,'VM_resumes':0,'database_connection_after_fix_tested':False}
(here/'pg-host-local-check.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
