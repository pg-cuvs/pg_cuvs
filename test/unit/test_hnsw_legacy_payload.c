#define _GNU_SOURCE 1
#include "cuvs_ipc.h"
#include "cuvs_build_corpus.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Actual-daemon probe: v1's 16-byte prefix must be rejected before waiting
 * for current payload fields. No graph request is admitted, and no GPU build is needed. */
int main(int argc, char **argv)
{
    assert(argc == 3);
    int sock=socket(AF_UNIX,SOCK_STREAM,0),owner[2],result=-1;
    assert(sock>=0 && socketpair(AF_UNIX,SOCK_STREAM,0,owner)==0);
    struct timeval timeout={2,0};
    assert(!setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof(address.sun_path),"%s",argv[1]);
    assert(!connect(sock,(struct sockaddr *)&address,sizeof(address)));
    char path[4096];snprintf(path,sizeof(path),"%s/legacy-payload-XXXXXX",argv[2]);
    int fd=mkstemp(path);assert(fd>=0);unlink(path);
    assert(!ftruncate(fd,100*128*4));
    CuvsCmdFrame cmd={0};
    cmd.proto_magic=CUVS_PROTO_MAGIC;cmd.proto_version=CUVS_PROTO_VERSION;
    cmd.op=CUVS_OP_BUILD_HNSW_FILE;cmd.n_vecs=100;cmd.dim=128;
    cmd.graph_degree=64;cmd.intermediate_graph_degree=128;
    assert(write(sock,&cmd,sizeof(cmd))==sizeof(cmd));
    struct {uint32_t version,hierarchy;uint64_t host_budget;} legacy={1,0,1ULL<<30};
    assert(!cuvs_fd_send(sock,fd,&legacy,sizeof(legacy)));
    char token='O';assert(!cuvs_fd_send(sock,owner[1],&token,1));
    assert(read(sock,&token,1)==1 && token=='R');
    CuvsReplyHeader reply={0};
    assert(!cuvs_fd_recv(sock,&reply,sizeof(reply),&result));
    assert(reply.status==CUVS_STATUS_PROTO_MISMATCH && result==-1);
    assert(strstr(reply.error,"payload version mismatch"));
    close(fd);close(sock);close(owner[0]);close(owner[1]);
    printf("PASS actual daemon rejects v1 HNSW payload promptly; current global v%u retained; GPU build0\n", CUVS_PROTO_VERSION);
    return 0;
}
