import json
import subprocess
import sys
from pathlib import Path
import psycopg

repo, out, initial, percent = sys.argv[1:]
initial, percent = int(initial), int(percent)
conn = psycopg.connect(dbname='index_accel_smoke', host='/var/run/postgresql', autocommit=True)
conn.execute("SET statement_timeout = '20s'")
values = {key: conn.execute('SELECT current_setting(%s)', (key,)).fetchone()[0]
          for key in ('cuvs.rmm_pool_mb', 'cuvs.rmm_pool_max_percent', 'cuvs.rmm_pool_status',
                      'cuvs.itopk_size', 'cuvs.search_width')}
status = json.loads(values['cuvs.rmm_pool_status'])
assert int(values['cuvs.rmm_pool_mb']) == status['initial_mb'] == initial
assert int(values['cuvs.rmm_pool_max_percent']) == status['maximum_percent'] == percent
assert values['cuvs.itopk_size'] == values['cuvs.search_width'] == '0'
if initial:
    assert len(status['devices']) == 1
    for dev in status['devices']:
        assert dev['device_id'] == 0
        assert 0 <= dev['used_bytes'] <= dev['reserved_bytes'] <= dev['maximum_bytes']
        assert dev['reserved_bytes'] >= initial * 1024 * 1024
else:
    assert status['devices'] == []
rejections = {}
for name, value in [('cuvs.rmm_pool_mb', '1024'), ('cuvs.rmm_pool_max_percent', '100'),
                    ('cuvs.rmm_pool_status', '{}')]:
    try:
        conn.execute('SELECT set_config(%s,%s,false)', (name, value))
    except psycopg.Error as exc:
        assert 'cannot be changed' in str(exc)
        rejections[name] = {'sqlstate': exc.sqlstate, 'message': str(exc)}
    else:
        raise AssertionError('read-only setting accepted: ' + name)
resident = conn.execute("SELECT count(*) FROM pg_stat_gpu_search WHERE index_oid='t_cagra'::regclass").fetchone()[0]
assert resident == (0 if percent == 1 else 1), resident
pid = conn.execute('SELECT pg_backend_pid()').fetchone()[0]
root_code = r'''
import os, json, hashlib, subprocess, sys
from pathlib import Path
repo = Path(sys.argv[2])
dpid = int(subprocess.check_output(['systemctl','show','-p','MainPID','--value','pg-cuvs-server']))
def digest(path):
    with open(path, 'rb') as stream:
        st = os.fstat(stream.fileno()); digest = hashlib.sha256()
        for block in iter(lambda: stream.read(8*1024*1024), b''):
            digest.update(block)
        h = digest.hexdigest()
    return {'sha256': h, 'bytes': st.st_size, 'device': st.st_dev, 'inode': st.st_ino}
daemon = digest('/proc/%d/exe' % dpid)
built_daemon = digest(repo/'pg_cuvs_server')
assert daemon['sha256'] == built_daemon['sha256']
libdir = subprocess.check_output(['/usr/lib/postgresql/16/bin/pg_config','--pkglibdir'],text=True).strip()
so = Path(libdir)/'pg_cuvs.so'
installed = digest(so); built_so = digest(repo/'pg_cuvs.so')
assert installed['sha256'] == built_so['sha256']
maps = [line for line in Path('/proc/%s/maps' % sys.argv[1]).read_text().splitlines() if str(so) in line]
assert maps and all('(deleted)' not in line for line in maps)
for line in maps:
    fields = line.split(); major, minor = (int(v,16) for v in fields[3].split(':'))
    assert os.makedev(major,minor) == installed['device'] and int(fields[4]) == installed['inode']
print(json.dumps({'daemon_pid':dpid,'backend_pid':int(sys.argv[1]),'running_exe':daemon,
                 'built_daemon':built_daemon,'installed_extension':installed,'built_extension':built_so,
                 'extension_maps':maps,'source_commit':subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()}))
'''
r = subprocess.run(['sudo', 'python3', '-c', root_code, str(pid), repo], capture_output=True, text=True, timeout=15)
if r.returncode:
    raise RuntimeError(r.stderr)
record = {'values': values, 'pool': status, 'readonly_rejections': rejections,
          'resident_original_index_count': resident, 'binding': json.loads(r.stdout)}
Path(out).write_text(json.dumps(record, indent=2) + '\n')
conn.close()
print(json.dumps(record, indent=2))
