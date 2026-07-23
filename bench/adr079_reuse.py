from __future__ import annotations

import hashlib
import struct
from typing import TYPE_CHECKING, Final

import numpy as np

if TYPE_CHECKING:
    from numpy.typing import NDArray

HASH_MOD: Final = 1_000_000
KNUTH: Final = 2_654_435_761


def corpus_fingerprint(base: NDArray[np.float32], count: int) -> str:
    vector_header = struct.pack("!hh", base.shape[1], 0)
    corpus_hash = hashlib.md5(usedforsecurity=False)

    for rid in range(count):
        category = (rid * KNUTH) % HASH_MOD
        vector_bytes = np.asarray(base[rid], dtype=">f4").tobytes()
        row_bytes = struct.pack("!qi", rid, category) + vector_header + vector_bytes
        row_hash = hashlib.md5(row_bytes, usedforsecurity=False)
        corpus_hash.update(row_hash.hexdigest().encode("ascii"))

    return corpus_hash.hexdigest()
