#ifndef CUVS_COVER_TESTGATE_H
#define CUVS_COVER_TESTGATE_H
/*
 * Regression builds only (-DCUVS_COVER_RECERT_TEST_GATE); the product build includes nothing from here.
 *
 * The test sets CUVS_RECERT_GATE_DIR and arms one gate by creating "<name>_armed" in it. A process that reaches the gate says so by
 * creating "<name>_reached", waits for "<name>_go" (at most 60 s) and returns the first character of "<name>_touch" if that file
 * exists (else 0). Without "<name>_armed" the gate does nothing. Gates: "walk" (the start of the reachability walk, after phase 1 released the
 * cover lock: inserts let through here are written before the walk reads the graph), "recert" (between the new chain and its publication),
 * "vacuum" (between the transition to LOST and pgvector's ambulkdelete), "insert" (between the ledger key and pgvector's insert), "extended"
 * (the index is extended for the new chain, the pages are not written yet), "sweep" (a sweep worker has judged an index and not yet acted on it),
 * "status" (the status function has read the descriptor and not yet the ledger chain).
 * CUVS_RECERT_GATE_GROWTH_PAGES replaces the limit of the growth monitor of an insert.
 * A process that waits at a gate answers cancel and terminate requests.
 */
#ifdef CUVS_COVER_RECERT_TEST_GATE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "miscadmin.h"

/* A process held at a gate leaves it when this says so (cuvs_cover_worker.c: a background worker that would step aside in a walk). */
extern bool (*cuvs_cover_test_gate_release) (void);

static inline char
cuvs_cover_test_gate(const char *name)
{
	const char *dir = getenv("CUVS_RECERT_GATE_DIR");
	char		path[MAXPGPATH];
	FILE	   *f;
	int			n;
	char		kind = 0;

	if (dir == NULL)
		return 0;
	snprintf(path, sizeof(path), "%s/%s_armed", dir, name);
	if (access(path, F_OK) != 0)
		return 0;
	snprintf(path, sizeof(path), "%s/%s_reached", dir, name);
	if ((f = fopen(path, "w")) != NULL)
		fclose(f);
	snprintf(path, sizeof(path), "%s/%s_go", dir, name);
	for (n = 0; n < 600 && access(path, F_OK) != 0; n++)
	{
		CHECK_FOR_INTERRUPTS();	/* a test can cancel or terminate the process that waits here */
		if (cuvs_cover_test_gate_release != NULL && cuvs_cover_test_gate_release())
			break;
		pg_usleep(100000);
	}
	snprintf(path, sizeof(path), "%s/%s_touch", dir, name);
	if ((f = fopen(path, "r")) != NULL)
	{
		kind = (char) fgetc(f);
		fclose(f);
	}
	return kind;
}

/* The growth monitor of an insert (CUVS_COVER_GROWTH_PAGES, 256) with a limit a test can reach with a small index. */
static inline int
cuvs_cover_test_growth_pages(int dflt)
{
	const char *e = getenv("CUVS_RECERT_GATE_GROWTH_PAGES");

	return e != NULL && atoi(e) > 0 ? atoi(e) : dflt;
}
#endif

#endif
