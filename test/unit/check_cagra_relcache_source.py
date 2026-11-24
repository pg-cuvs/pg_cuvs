"""Guard CAGRA insert metadata across calls that can discard relcache."""

from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_hnsw_explicit_source import function


ROOT = Path(__file__).resolve().parents[2]
DETOAST = "int dim = DatumGetPgVector(values[0])->dim;"
APPEND = "cuvs_cagra_append_pending(indexRel, heap_tid, dim);"
REFRESH = "cuvs_cagra_relation_cache(indexRel)->stale_sent = true;"
SNAPSHOT = "CuvsCagraRelCache cached = *cuvs_cagra_relation_cache(index);"
PUBLISH = "cache = cuvs_cagra_relation_cache(index);"


def check(insert, append):
    assert not re.search(r"CuvsCagraRelCache\s*\*", insert), "INSERT_RETAINS_POINTER"
    assert DETOAST in insert and APPEND in insert and insert.index(DETOAST) < insert.index(APPEND), (
        "APPEND_BEFORE_DETOAST"
    )
    assert REFRESH in insert and insert.index(REFRESH) > insert.index(
        "cuvs_mark_index_stale(indexRel);"
    ), "STALE_REUSES_POINTER"
    assert SNAPSHOT in append and append.index(SNAPSHOT) < append.index("ReadBuffer("), (
        "APPEND_READS_BORROWED_POINTER"
    )
    assert append.count("cuvs_cagra_relation_cache(index)") == 2, "APPEND_CACHE_ACQUISITIONS"
    assert PUBLISH in append and append.index(PUBLISH) > append.rindex("UnlockReleaseBuffer("), (
        "APPEND_PUBLISHES_EARLY"
    )
    store = append.index(PUBLISH)
    for field in ("index_nonce", "tail", "dim"):
        assert append.index(f"cache->{field} =") > store, "APPEND_WRITE_BEFORE_REFRESH"


def replace_once(body, before, after):
    assert body.count(before) == 1, f"mutation anchor count for {before!r}"
    return body.replace(before, after, 1)


def main():
    insert = function((ROOT / "src/pg_cuvs.c").read_text(), "cuvs_aminsert(Relation indexRel")
    append = function((ROOT / "src/cuvs_cagra_pages.c").read_text(),
                      "cuvs_cagra_append_pending(Relation index")
    check(insert, append)
    early_refresh = replace_once(append, PUBLISH, "")
    early_refresh = replace_once(early_refresh, "GenericXLogFinish(wal);",
                                 PUBLISH + "\n        GenericXLogFinish(wal);")
    mutants = (
        ("INSERT_RETAINS_POINTER",
         replace_once(insert, "PgVector *vec;",
                      "CuvsCagraRelCache *cache = cuvs_cagra_relation_cache(indexRel);\n    PgVector *vec;"),
         append),
        ("APPEND_BEFORE_DETOAST",
         replace_once(insert, DETOAST + "\n\n            " + APPEND,
                      APPEND + "\n            " + DETOAST),
         append),
        ("STALE_REUSES_POINTER", replace_once(insert, REFRESH, "cache->stale_sent = true;"), append),
        ("APPEND_READS_BORROWED_POINTER",
         insert, replace_once(append, SNAPSHOT, "CuvsCagraRelCache *cache = cuvs_cagra_relation_cache(index);")),
        ("APPEND_PUBLISHES_EARLY", insert, early_refresh),
    )
    for expected, mutated_insert, mutated_append in mutants:
        try:
            check(mutated_insert, mutated_append)
        except AssertionError as error:
            assert str(error) == expected, f"expected {expected}, got {error}"
            print(f"RED {expected}")
        else:
            raise AssertionError(f"source mutation survived: {expected}")
    print(f"CAGRA relcache source: baseline and {len(mutants)} detected mutations")


if __name__ == "__main__":
    main()
