/*
 * I3-B daemon op client for the CPU-shim integration scenario
 * (test/i3b/run_i3b_shim.sh). Not installed.
 *
 *   i3b_ipc_cli <socket> unload|load|pin|unpin <db_oid> <index_oid>
 *   i3b_ipc_cli <socket> set <name> <value>
 *   i3b_ipc_cli <socket> index|memory|reclaim
 * Prints "status=<n> <text>"; exit 0 iff status OK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuvs_ipc.h"

int
main(int argc, char **argv)
{
    char msg[256];
    int rc;
    if (argc < 3)
        return 2;
    const char *sock = argv[1], *op = argv[2];
    if (argc == 5 && strcmp(op, "set") != 0)
    {
        uint32_t code = strcmp(op, "unload") == 0 ? CUVS_OP_UNLOAD
                      : strcmp(op, "load") == 0 ? CUVS_OP_LOAD
                      : strcmp(op, "pin") == 0 ? CUVS_OP_PIN : CUVS_OP_UNPIN;
        rc = cuvs_ipc_manual(sock, code, (uint32_t)strtoul(argv[3], NULL, 10),
                             (uint32_t)strtoul(argv[4], NULL, 10), msg, sizeof(msg));
        printf("status=%d %s\n", rc, msg);
        return rc == CUVS_STATUS_OK ? 0 : 1;
    }
    if (argc == 5)
    {
        rc = cuvs_ipc_reclaim_set(sock, argv[3], argv[4], msg, sizeof(msg));
        printf("status=%d %s\n", rc, msg);
        return rc == CUVS_STATUS_OK ? 0 : 1;
    }
    char *text = NULL;
    uint32_t code = strcmp(op, "index") == 0 ? CUVS_OP_INDEX_STATUS
                  : strcmp(op, "memory") == 0 ? CUVS_OP_MEMORY_STATUS : CUVS_OP_RECLAIM_STATUS;
    rc = cuvs_ipc_text(sock, code, &text, NULL);
    printf("status=%d\n%s\n", rc, text ? text : "");
    free(text);
    return rc == CUVS_STATUS_OK ? 0 : 1;
}
