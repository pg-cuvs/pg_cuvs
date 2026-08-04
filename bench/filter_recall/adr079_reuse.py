from __future__ import annotations

import hashlib
import struct
from typing import TYPE_CHECKING, Final

import numpy as np

if TYPE_CHECKING:
    from numpy.typing import NDArray

HASH_MOD: Final = 1_000_000
KNUTH: Final = 2_654_435_761


def corpus_fingerprint(
    base: NDArray[np.float32], count: int, has_category: bool = True,
    id_offset: int = 0,
) -> str:
    """md5-of-ordered-row-md5, byte-compatible with the PG wire format the row
    came from. With has_category=True (default, used by the f3o table which
    carries a synthetic `cat` column): int8send(id) || int4send(cat) ||
    vector_send(embedding). With has_category=False (tables with no `cat`
    column, e.g. cuvs_bench_backend's plain corpus table `t`): int8send(id) ||
    vector_send(embedding).

    id_offset (default 0, existing callers unaffected) lets a caller fingerprint
    a slice of a larger corpus while using the slice's true (global) row ids in
    the hash -- e.g. to fold a large corpus into per-chunk digests without
    holding the whole array in memory at once."""
    vector_header = struct.pack("!hh", base.shape[1], 0)
    corpus_hash = hashlib.md5(usedforsecurity=False)

    for i in range(count):
        rid = id_offset + i
        vector_bytes = np.asarray(base[i], dtype=">f4").tobytes()
        if has_category:
            category = (rid * KNUTH) % HASH_MOD
            prefix = struct.pack("!qi", rid, category)
        else:
            prefix = struct.pack("!q", rid)
        row_bytes = prefix + vector_header + vector_bytes
        row_hash = hashlib.md5(row_bytes, usedforsecurity=False)
        corpus_hash.update(row_hash.hexdigest().encode("ascii"))

    return corpus_hash.hexdigest()
