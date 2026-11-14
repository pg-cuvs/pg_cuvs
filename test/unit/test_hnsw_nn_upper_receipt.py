#!/usr/bin/env python3
"""Exercise the production build_info NN JSON branch on indistinguishable stored receipts."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=root/'src/hnsw_export.c');a=p.parse_args()
source=a.source.read_text()
start=source.index('    if (policy.route == CUVS_HNSW_ROUTE_NN_DESCENT)',source.index('pg_cuvs_hnsw_build_info(PG_FUNCTION_ARGS)'))
end=source.index("    appendStringInfoChar(&json, '}');",start)
branch=source[start:end]
prefix=r'''
#include "cuvs_hnsw_policy.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef struct { char data[2048]; size_t len; } Buffer;
static void appendStringInfo(Buffer *p, const char *fmt, ...) {
 va_list a;va_start(a,fmt);int n=vsnprintf(p->data+p->len,sizeof(p->data)-p->len,fmt,a);va_end(a);
 assert(n>=0 && (size_t)n<sizeof(p->data)-p->len);p->len+=(size_t)n;
}
static Buffer render(CuvsHnswPolicy policy,CuvsHnswNnReadback nn) { Buffer json={{0},0};
'''
suffix=r'''
 return json;
}
struct Page { CuvsHnswPolicy policy; CuvsHnswNnReadback nn; };
static struct Page stored(unsigned dtype, unsigned hierarchy) {
 struct Page b={0};b.policy.magic=CUVS_HNSW_POLICY_MAGIC;b.policy.version=CUVS_HNSW_POLICY_NN_VERSION;
 b.policy.bytes=sizeof(b.policy);b.policy.route=CUVS_HNSW_ROUTE_NN_DESCENT;
 b.policy.requested_algo=CUVS_HNSW_ALGO_NN_DESCENT;b.policy.requested_ef=64;b.policy.degree=64;
 b.policy.intermediate=128;b.policy.guard_passed=1;b.policy.internal_builder=CUVS_HNSW_ROUTE_NN_DESCENT;
 b.policy.hierarchy=hierarchy;b.policy.n_rows=10000;b.policy.dim=128;
 b.nn.magic=CUVS_HNSW_NN_READBACK_MAGIC;b.nn.version=1;b.nn.bytes=sizeof(b.nn);
 b.nn.graph_degree=128;b.nn.internal_degree=192;b.nn.sdk_default_iterations=20;b.nn.max_iterations=20;
 b.nn.dtype=dtype;b.nn.termination_threshold=0.0001f;return b;
}
int main(void) {
 struct Page r1=stored(CUVS_HNSW_NN_DTYPE_FP32,1),r2=stored(CUVS_HNSW_NN_DTYPE_FP32,1);
 CuvsHnswPolicy policy;assert(!memcmp(&r1,&r2,sizeof(r1)));
 for (unsigned i=0;i<3;i++) {
  struct Page page=i==0?r1:i==1?r2:stored(CUVS_HNSW_NN_DTYPE_FP16,1);
  assert(cuvs_hnsw_policy_read(&page,sizeof(page),&policy)==CUVS_HNSW_POLICY_RECORDED);
  Buffer out=render(policy,page.nn);
  if(!strstr(out.data,"\"nn_upper_dtype_configured\":null")) {fprintf(stderr,"unrecorded upper configured dtype inferred: %s\n",out.data);return 2;}
  assert(strstr(out.data,"\"nn_actual_iterations\":null"));
  assert(strstr(out.data,"\"nn_upper_dtype\":\"per_level_not_recorded\""));
 }
 struct Page nsw=stored(CUVS_HNSW_NN_DTYPE_FP32,0);
 assert(cuvs_hnsw_policy_read(&nsw,sizeof(nsw),&policy)==CUVS_HNSW_POLICY_RECORDED);
 Buffer out=render(policy,nsw.nn);assert(strstr(out.data,"\"nn_upper_dtype_configured\":\"not_applicable\""));
 puts("r1/r2 stored NN upper configured dtype unknown; nsw not applicable: PASS (CPU fixture)");
}
'''
with tempfile.TemporaryDirectory(prefix='pgcuvs-upper-receipt-') as tmp:
 d=Path(tmp);(d/'test.c').write_text(prefix+branch+suffix)
 compiled=subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(root/'src'),str(d/'test.c'),'-o',str(d/'test')],capture_output=True,text=True)
 if compiled.returncode:raise RuntimeError('fixture compile failed: '+compiled.stderr)
 result=subprocess.run([str(d/'test')],capture_output=True,text=True)
 print(result.stdout,end='');print(result.stderr,end='')
 raise SystemExit(result.returncode)
