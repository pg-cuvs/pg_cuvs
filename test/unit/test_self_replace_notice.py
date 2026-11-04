"""Execute the daemon's real notice/reset/reply bodies; capture only logging/I/O."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
server = (ROOT / 'src/pg_cuvs_server.c').read_text()
policy = (ROOT / 'src/cuvs_cache_policy.c').read_text()
if '--mutate' in sys.argv:
    needle = 'self_unloaded == 0 || t_self_replace_failure_noted'
    assert server.count(needle) == 1
    server = server.replace(needle, 'self_unloaded == 0')


def function(source, name):
    start = source.index('\n' + name + '(') + 1
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


decls = server[server.index('static __thread char g_build_notice'):
               server.index('\nstatic void\nclear_build_notices')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CUVS_NOTICE_OUT_BYTES 1024
#define CUVS_STATUS_OK 0
typedef struct { int status; char notice_out[1024]; } CuvsReplyHeader;
static char t_evict_notices[1024];
static unsigned logged, appended;
#define LOG_INFO(...) (++logged)
static void append_notice_logged(CuvsReplyHeader *h, const char *m, const char *d)
{ (void)h; (void)d; if (*m) ++appended; }
''' + decls
code += '\nint\n' + function(policy, 'cuvs_self_replace_fail_notice')
for name in ['clear_build_notices', 'note_self_replace_failure', 'attach_build_notices']:
    code += '\nstatic void\n' + function(server, name)
code += r'''
int main(void) {
    clear_build_notices();
    t_notice_index_oid = 42; t_notice_self_unloaded = 100;
    note_self_replace_failure(42, 100); /* build wrapper observes failure */
    CuvsReplyHeader h = {.status = 1};
    attach_build_notices(&h); /* error reply observes the same failure */
    assert(logged == 1);
    assert(appended == 1);
    assert(strstr(g_build_notice, "index 42 failed") != NULL);
    assert(strcmp(g_build_notice_detail,
                  "The next query on this index reloads the old copy") == 0);
    clear_build_notices();
    t_notice_index_oid = 43; t_notice_self_unloaded = 200;
    attach_build_notices(&h); /* generic failure must still create the notice */
    assert(logged == 2 && appended == 2);
    assert(strstr(g_build_notice, "index 43 failed") != NULL);
    clear_build_notices();
    note_self_replace_failure(44, 0);
    assert(logged == 2 && g_build_notice[0] == 0);
    puts("PASS same-request once, next-request reset, generic reply, zero unload");
}
'''
with tempfile.TemporaryDirectory(prefix='self-replace-notice-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(source),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
