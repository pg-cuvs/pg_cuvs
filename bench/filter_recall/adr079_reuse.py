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
) -> str:
    """md5-of-ordered-row-md5, byte-compatible with the PG wire format the row
    came from. With has_category=True (default, used by the f3o table which
    carries a synthetic `cat` column): int8send(id) || int4send(cat) ||
    vector_send(embedding). With has_category=False (tables with no `cat`
    column, e.g. cuvs_bench_backend's plain corpus table `t`): int8send(id) ||
    vector_send(embedding)."""
    vector_header = struct.pack("!hh", base.shape[1], 0)
    corpus_hash = hashlib.md5(usedforsecurity=False)

    for rid in range(count):
        vector_bytes = np.asarray(base[rid], dtype=">f4").tobytes()
        if has_category:
            category = (rid * KNUTH) % HASH_MOD
            prefix = struct.pack("!qi", rid, category)
        else:
            prefix = struct.pack("!q", rid)
        row_bytes = prefix + vector_header + vector_bytes
        row_hash = hashlib.md5(row_bytes, usedforsecurity=False)
        corpus_hash.update(row_hash.hexdigest().encode("ascii"))

    return corpus_hash.hexdigest()
