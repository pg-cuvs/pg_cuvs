/* Direct production workspace reservation, with syscall observation only. */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static unsigned calls;
static int last_rc, all_masked;
static bool inject_eintr;
static int observed_fallocate(int fd,off_t offset,off_t bytes)
{
    sigset_t mask;
    char line[96];
    sigprocmask(SIG_SETMASK,NULL,&mask);
    all_masked=all_masked && sigismember(&mask,SIGUSR1)==1;
    calls++;
    int n=snprintf(line,sizeof(line),"CONN203_FALLOC_BEGIN pid=%d\n",(int)getpid());
    /* No ereport/NOTICE here: its CFI could throw while the raw fd is live. */
    (void)write(STDERR_FILENO,line,(size_t)n);
    last_rc=inject_eintr && calls==1 ? EINTR : posix_fallocate(fd,offset,bytes);
    return last_rc;
}
#define posix_fallocate observed_fallocate
#include "hnsw_export.c"
#undef posix_fallocate

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(conn203_workspace_probe);
Datum conn203_workspace_probe(PG_FUNCTION_ARGS)
{
    size_t bytes=(size_t)PG_GETARG_INT64(0);
    sigset_t before,after;
    bool spilled,ok,restored=true;
    calls=0;last_rc=-1;all_masked=1;
    inject_eintr=PG_GETARG_BOOL(1);
    sigprocmask(SIG_SETMASK,NULL,&before);
    void *p=hnsw_connectivity_workspace(bytes,&spilled);
    ok=p!=NULL;
    hnsw_connectivity_workspace_free(p,bytes,spilled);
    sigprocmask(SIG_SETMASK,NULL,&after);
    for(int i=1;i<NSIG;i++)if(sigismember(&before,i)!=sigismember(&after,i))restored=false;
    CHECK_FOR_INTERRUPTS();
    PG_RETURN_TEXT_P(cstring_to_text(psprintf(
        "{\"allocated\":%s,\"spilled\":%s,\"calls\":%u,\"last_rc\":%d,\"all_sigusr1_masked\":%s,\"mask_restored\":%s,\"log_memory_pending\":%s}",
        ok?"true":"false",spilled?"true":"false",calls,last_rc,
        all_masked?"true":"false",restored?"true":"false",LogMemoryContextPending?"true":"false")));
}
