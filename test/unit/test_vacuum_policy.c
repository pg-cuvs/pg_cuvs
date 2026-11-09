/* amvacuumcleanup policy (cuvs_vacuum_policy.h): an ANALYZE (analyze_only,
 * including autoanalyze) never considers compaction; a VACUUM does when a
 * daemon socket is configured, as before (the tombstone and stale checks
 * that follow are unchanged in pg_cuvs.c). Run 20261109-q-03 QV V11 saw a
 * COMPACT with no runner command: autoanalyze reached the compaction. */
#include <stdbool.h>
#include <stdio.h>

#include "cuvs_vacuum_policy.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* ANALYZE / autoanalyze: never, with or without a daemon. */
    CHECK(!cuvs_vacuum_cleanup_considers_compact(true, true));
    CHECK(!cuvs_vacuum_cleanup_considers_compact(true, false));
    /* VACUUM: considered when a daemon socket is configured (unchanged). */
    CHECK(cuvs_vacuum_cleanup_considers_compact(false, true));
    CHECK(!cuvs_vacuum_cleanup_considers_compact(false, false));

    if (failures) {
        fprintf(stderr, "test_vacuum_policy: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS vacuum policy: analyze_only never compacts; VACUUM with a daemon considers compaction\n");
    return 0;
}
