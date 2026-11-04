"""Compile the real daemon OOM logger; consume its emitted bytes with CK-10."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/pg_cuvs_server.c').read_text()
start = source.index('\nlog_build_oom(') + 1
end = source.index('\n}', start) + 2
body = source[start:end]
if '--mutate-drop' in sys.argv:
    assert body.count(' failed_bytes=%lld') == 1
    body = body.replace(' failed_bytes=%lld', '')
if '--mutate-zero' in sys.argv:
    assert body.count('= cuvs_last_failed_allocation_bytes();') == 1
    body = body.replace('= cuvs_last_failed_allocation_bytes();', '= 0;')
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
static int64_t failed = 4096;
int64_t cuvs_last_failed_allocation_bytes(void) { int64_t v=failed; failed=-1; return v; }
static uint64_t admission_mono_us(void) { return 100; }
static void record(const char *format, ...) {
 va_list args; va_start(args,format); vprintf(format,args); va_end(args);
}
#define LOG_WARN(...) record(__VA_ARGS__)
''' + '\nstatic void\n' + body + r'''
int main(void) {
 log_build_oom("BUILD",0,1,2,800);
 log_build_oom("BUILD",0,1,3,800);
}
'''
spec = importlib.util.spec_from_file_location('judge',root/'test/unit/check_i3_logs.py')
judge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(judge)
with tempfile.TemporaryDirectory(prefix='build-oom-log-') as tmp:
    c, binary = Path(tmp)/'test.c', Path(tmp)/'test'
    c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
    text = subprocess.check_output([str(binary)],text=True)
    print(text,end='')
    rows = [judge.parse_line(line)[1] for line in text.splitlines()]
    assert len(rows)==2 and [r['failed_bytes'] for r in rows]==['4096','-1']
    assert all(r['needed']=='800' for r in rows)
    print('PASS actual logger eight fields, upstream X != needed, unknown -1')
