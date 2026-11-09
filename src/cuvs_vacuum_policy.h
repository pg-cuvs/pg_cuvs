#ifndef CUVS_VACUUM_POLICY_H
#define CUVS_VACUUM_POLICY_H

/* amvacuumcleanup policy, PostgreSQL-free so the CPU unit test runs it.
 *
 * PostgreSQL calls amvacuumcleanup after VACUUM and also from ANALYZE
 * (including autoanalyze), with IndexVacuumInfo.analyze_only set. Index AMs
 * do no cleanup work in that case (btree returns at once). pg_cuvs's cleanup
 * work is the tombstone compaction (69f31f3): after a VACUUM, an index that
 * has a .tombstone file is sent COMPACT. An ANALYZE must not compact, so the
 * cleanup considers compaction only when analyze_only is false and a daemon
 * socket is configured. cuvs.auto_compact is not an input: it controls the
 * background REINDEX on delta growth, not VACUUM compaction.
 */

#include <stdbool.h>

static inline bool
cuvs_vacuum_cleanup_considers_compact(bool analyze_only, bool daemon_configured)
{
    return !analyze_only && daemon_configured;
}

#endif /* CUVS_VACUUM_POLICY_H */
