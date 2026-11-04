"""Run the actual build-end logger with physical pool/meter fixtures."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/pg_cuvs_server.c').read_text()
start = source.index('\nlog_build_end(') + 1
body = source.index('{', start)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]
if '--mutate' in sys.argv:
    needle = 'g_rmm_pool_limits[a->gpu] > 0 && g_reclaim.peak_meter'
    assert function.count(needle) == 1
    function = function.replace(needle, 'g_rmm_pool_limits[a->gpu] > 0')

code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cuvs_wrapper.h"
#include "cuvs_lw_policy.h"
typedef struct {
 const char *op, *algo;
 int gpu, workspace_valid;
 struct { int managed; } workspace;
 unsigned db, idx, dim, gd, igd;
 size_t final; int64_t n;
 uint64_t used_before, build_started_us, build_finished_us, self_res;
} AdmitLine;
static uint64_t g_rmm_pool_limits[1];
static unsigned g_pending_build_count[1];
static struct { int peak_meter; } g_reclaim;
static unsigned peaks, results;
static void capture(const char *format, ...) {
 if (strstr(format, "[build_peak]")) ++peaks;
 if (strstr(format, "[build_result]")) ++results;
}
#define LOG_INFO(...) capture(__VA_ARGS__)
int cuvs_rmm_pool_stats(int gpu, CuvsRmmPoolStats *s) {
 (void)gpu; s->used_bytes=100; s->reserved_bytes=200; return 0;
}
size_t cuvs_rmm_pool_peak_bytes(int gpu) { (void)gpu; return 100; }
static uint64_t admission_mono_us(void) { return 1000; }
'''
code += '\nstatic void\n' + function
code += r'''
int main(void) {
 AdmitLine a = {.op="BUILD", .algo="cagra", .gpu=0};
 const char *outcomes[] = {"built", "failed", "oom"};
 for (unsigned i=0; i<3; ++i) {
  g_rmm_pool_limits[0]=1000; g_reclaim.peak_meter=1;
  log_build_end(&a, NULL, 1, outcomes[i]);
  assert(peaks==i+1 && results==3*i+1);
  g_reclaim.peak_meter=0;
  log_build_end(&a, NULL, 1, outcomes[i]);
  assert(peaks==i+1 && results==3*i+2);
  g_rmm_pool_limits[0]=0; g_reclaim.peak_meter=1;
  log_build_end(&a, NULL, 1, outcomes[i]);
  assert(peaks==i+1 && results==3*i+3);
 }
 puts("PASS build/OOM/failure: peak on, peak off, pool off; results retained");
}
'''
with tempfile.TemporaryDirectory(prefix='peak-log-switch-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                    str(root / 'src'), str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
