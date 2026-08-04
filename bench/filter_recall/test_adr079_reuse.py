import hashlib
import struct

import numpy as np
from adr079_3o_recall import exact_topk_in_subset
from adr079_reuse import corpus_fingerprint
from pgvector import Vector


def test_corpus_fingerprint_matches_postgres_wire_contract() -> None:
    # Given rows whose expected payload uses pgvector's independent binary codec.
    base = np.asarray([[1.0, 2.0], [-3.5, 4.25]], dtype=np.float32)
    row_hashes: list[str] = []
    for rid, row in enumerate(base):
        category = (rid * 2_654_435_761) % 1_000_000
        payload = struct.pack("!qi", rid, category) + Vector(row).to_binary()
        row_hashes.append(
            hashlib.md5(payload, usedforsecurity=False).hexdigest(),
        )

    # When the benchmark computes the corpus fingerprint.
    actual = corpus_fingerprint(base, len(base))

    # Then it matches PostgreSQL's ordered md5-of-row-md5 aggregate contract.
    expected = hashlib.md5(
        "".join(row_hashes).encode("ascii"),
        usedforsecurity=False,
    ).hexdigest()
    assert actual == expected


def test_corpus_fingerprint_no_category_matches_id_plus_vector_wire_contract() -> None:
    # Given rows from a table with no `cat` column (cuvs_bench_backend's plain
    # corpus table `t` -- #78 issue comment: "Table t has no cat column, so
    # drop that term").
    base = np.asarray([[1.0, 2.0], [-3.5, 4.25]], dtype=np.float32)
    row_hashes: list[str] = []
    for rid, row in enumerate(base):
        payload = struct.pack("!q", rid) + Vector(row).to_binary()
        row_hashes.append(
            hashlib.md5(payload, usedforsecurity=False).hexdigest(),
        )

    # When the benchmark computes the corpus fingerprint with has_category=False.
    actual = corpus_fingerprint(base, len(base), has_category=False)

    # Then it matches PostgreSQL's ordered md5-of-row-md5 aggregate contract for
    # int8send(id) || vector_send(embedding), with no category term, and differs
    # from the has_category=True (default) fingerprint for the same rows.
    expected = hashlib.md5(
        "".join(row_hashes).encode("ascii"),
        usedforsecurity=False,
    ).hexdigest()
    assert actual == expected
    assert actual != corpus_fingerprint(base, len(base))


def test_exact_topk_handles_subset_equal_to_k() -> None:
    # Given a filtered subset containing exactly k rows.
    base = np.asarray([[0.0], [1.0], [2.0]], dtype=np.float32)
    subset = np.asarray([1, 2], dtype=np.int64)
    queries = np.asarray([[1.25]], dtype=np.float32)

    # When exact filtered top-k is requested.
    result = exact_topk_in_subset(base, subset, queries, 2)

    # Then both members are ranked without an out-of-range kth.
    assert result.tolist() == [[1, 2]]


def test_exact_topk_returns_all_members_when_subset_is_smaller_than_k() -> None:
    # Given fewer filtered rows than the requested k.
    base = np.asarray([[0.0], [1.0], [2.0]], dtype=np.float32)
    subset = np.asarray([2], dtype=np.int64)
    queries = np.asarray([[0.0]], dtype=np.float32)

    # When exact filtered top-k is requested.
    result = exact_topk_in_subset(base, subset, queries, 2)

    # Then ground truth contains every available member and no padding.
    assert result.tolist() == [[2]]
