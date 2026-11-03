#ifndef CUVS_ADMISSION_CONTRACT_H
#define CUVS_ADMISSION_CONTRACT_H

#include <stdint.h>

/* I3 shared contract: UC-2 r18. This is a text payload, not a wire struct.
 * The I3-A integration must bind CUVS_PROTO_VERSION to this version before
 * deploying the new producer and consumer together (ADR-081).
 */
#define CUVS_ADMISSION_IPC_VERSION 5u
#define CUVS_ADMISSION_ERR_OUT_BYTES 512u /* includes terminating NUL */
#define CUVS_ADMISSION_MAX_CANDIDATES 3u
#define CUVS_ADMISSION_SCALAR_MAX UINT32_C(99999999)
#define CUVS_ADMISSION_SHARD_MAX 999u
#define CUVS_ADMISSION_IDLE_NEVER (-1)
#define CUVS_ADMISSION_WAIT_QUERIES 0
#define CUVS_ADMISSION_WAIT_UNPIN (-1)

/* Enum numbers are local discriminants, not CuvsReplyHeader.status values.
 * The wire reason is the spelling AR0..AR7, OOM, or ATT. NONE is not sent.
 */
typedef enum CuvsAdmissionReason {
    CUVS_ADMISSION_NONE = -1,
    CUVS_ADMISSION_AR0 = 0,
    CUVS_ADMISSION_AR1,
    CUVS_ADMISSION_AR2,
    CUVS_ADMISSION_AR3,
    CUVS_ADMISSION_AR4,
    CUVS_ADMISSION_AR5,
    CUVS_ADMISSION_AR6,
    CUVS_ADMISSION_AR7,
    CUVS_ADMISSION_OOM,
    CUVS_ADMISSION_ATT
} CuvsAdmissionReason;

#define CUVS_ADMISSION_SQLSTATE_LIMIT "54000"     /* AR0 */
#define CUVS_ADMISSION_SQLSTATE_MEMORY "53200"    /* AR1..5, AR7, OOM, ATT */
#define CUVS_ADMISSION_SQLSTATE_WORKSPACE "53400" /* AR6 */

/* Field keys: byte-valued metrics are encoded in MiB, saturated at
 * CUVS_ADMISSION_SCALAR_MAX. OIDs keep their full uint32 range. A paired
 * value is MiB/count. Absent fields are absent, not a measured zero.
 * Per-operation required subsets remain in UC-2/C5.
 */
#define CUVS_ADMISSION_KEY_NEEDED "n"
#define CUVS_ADMISSION_KEY_FREE "f"
#define CUVS_ADMISSION_KEY_QUOTA "q"
#define CUVS_ADMISSION_KEY_PENDING "p"
#define CUVS_ADMISSION_KEY_RESIDENT "h"
#define CUVS_ADMISSION_KEY_EVICTABLE "e"
#define CUVS_ADMISSION_KEY_PINNED "k"
#define CUVS_ADMISSION_KEY_WORKSPACE "l"
#define CUVS_ADMISSION_KEY_LW_PENDING "y"
#define CUVS_ADMISSION_KEY_PHYSICAL_FREE "u"
#define CUVS_ADMISSION_KEY_HOST_GUARD "g"
#define CUVS_ADMISSION_KEY_SELF_RESIDENT "s"
#define CUVS_ADMISSION_KEY_WAIT "w"
#define CUVS_ADMISSION_KEY_RECLAIM "r"
#define CUVS_ADMISSION_KEY_CANDIDATE "t"
#define CUVS_ADMISSION_KEY_SHARD "i"

/* Text grammar (only the reason-specific subset is emitted):
 * ARx n=MiB f=MiB q=MiB p=MiB h=MiB/count e=MiB/count k=MiB/count
 *     l=MiB y=MiB u=MiB g=MiB s=MiB r=0|1
 *     [t=db_oid:index_oid:MiB:idle_s] (at most three) [w=0|-1] [i=0..999]
 * Candidates are unpinned, not in use, oldest last_use first. A fourth
 * candidate is an error, never silent truncation. idle_s=-1 means never.
 * w is not an estimated countdown: -1 requires unpin; 0 awaits queries.
 * The daemon alone computes r: headroom mode AND eligible amount > 0 AND
 * free+eligible >= need AND need <= Fcap AND post-eviction resident >= floor.
 * Eligibility includes EV-1, EV-2 and the sidecar exclusion. The backend
 * must not recompute F. A false or missing r never authorizes a wait hint.
 */
typedef struct CuvsAdmissionCandidate {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t mib;
    int32_t idle_s;
} CuvsAdmissionCandidate;

typedef struct CuvsAdmissionPayload {
    char text[CUVS_ADMISSION_ERR_OUT_BYTES];
} CuvsAdmissionPayload;

#endif /* CUVS_ADMISSION_CONTRACT_H */
