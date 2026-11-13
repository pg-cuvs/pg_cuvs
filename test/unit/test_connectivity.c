#include <assert.h>
#include <stdio.h>
#include "cuvs_connectivity.h"
static uint32_t edges[4][2][4];
static const unsigned char *links(void *ctx,uint32_t u,uint32_t l,uint32_t *n) {
 if(ctx && u==2 && l==1){*n=UINT32_MAX;return NULL;}
 *n=edges[u][l][0];return (unsigned char*)&edges[u][l][1];
}
static int cancel(void *ctx){(void)ctx;return 1;}
int main(void) {
 CuvsConnectivity s;
 edges[0][0][0]=1;edges[0][0][1]=1;
 edges[2][0][0]=1;edges[2][0][1]=3;
 edges[0][1][0]=1;edges[0][1][1]=2;
 assert(cuvs_connectivity(4,0,1,1,links,NULL,NULL,&s));
 assert(s.unreachable==0 && s.unreachable_l0==2 && s.wcc_l0==2 && s.wcc_all==1);
 assert(!cuvs_connectivity(4,0,1,1,links,(void*)1,NULL,&s));
 assert(cuvs_connectivity(4,0,0,1,links,NULL,NULL,&s));
 assert(s.unreachable==2 && s.wcc_all==1); /* No upward traversal at a non-entry node. */
 memset(edges,0,sizeof(edges));edges[0][0][0]=1;edges[0][0][1]=1;edges[1][0][0]=1;edges[1][0][1]=2;edges[3][0][0]=1;edges[3][0][1]=1;
 assert(cuvs_connectivity(4,0,0,1,links,NULL,NULL,&s));
 assert(s.unreachable==1 && s.wcc_all==1);
 edges[3][0][1]=99;assert(!cuvs_connectivity(4,0,0,1,links,NULL,NULL,&s));
 edges[3][0][1]=1;assert(!cuvs_connectivity(4,0,0,1,links,NULL,cancel,&s));
 assert(!cuvs_connectivity(0,0,0,0,links,NULL,NULL,&s));
 puts("connectivity: layered reach, directed miss, WCC, invalid edge, cancellation PASS");return 0;
}
