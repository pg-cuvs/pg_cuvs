/* Test-only MARK_STALE client: exercise a selected scan without plan invalidation. */
#include "cuvs_ipc.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof(addr.sun_path)) return 2;
    strcpy(addr.sun_path, argv[1]);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) return 3;
    CuvsCmdFrame cmd = {0};
    CuvsReplyHeader reply;
    cmd.proto_magic = CUVS_PROTO_MAGIC;
    cmd.proto_version = CUVS_PROTO_VERSION;
    cmd.op = CUVS_OP_MARK_STALE;
    cmd.db_oid = strtoul(argv[2], NULL, 10);
    cmd.index_oid = strtoul(argv[3], NULL, 10);
    if (send(fd, &cmd, sizeof(cmd), MSG_NOSIGNAL) != sizeof(cmd)) return 4;
    if (recv(fd, &reply, sizeof(reply), MSG_WAITALL) != sizeof(reply)) return 5;
    close(fd);
    return reply.status == CUVS_STATUS_OK ? 0 : 6;
}
