#ifndef CUVS_HNSW_RLS_H
#define CUVS_HNSW_RLS_H

void cuvs_hnsw_rls_init(void);
bool cuvs_hnsw_rls_query_active(void);
/* True when a table of the statement that is running has row-level security, whatever table the scan reads: a partitioned
 * or inheritance parent stays in the range table of the final plan, and its policy reaches the rows of its children. */
bool cuvs_hnsw_rls_statement_has_rls_relation(void);
/* True when the executor hooks have a statement for the scan that is running. A scan that is asked to evaluate the expression or
 * the predicate of an index and has no statement must not run: nothing says which rows the statement may see. */
bool cuvs_hnsw_rls_statement_known(void);

#endif
