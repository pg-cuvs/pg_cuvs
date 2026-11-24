#!/usr/bin/env python3
"""Check the SDK source files directly used by the experimental fold object."""
import argparse
import hashlib
from pathlib import Path

EXPECTED = {
    "neighbors/detail/cagra/add_nodes.cuh": "87ceffbb7cd44c3e017028dd3aa4f2bc1788af8491b53018d229e4ec475ce638",
    "neighbors/detail/ann_utils.cuh": "7a67d3858e0a467fc8497b0ae13e2c489044496864ba563027089db6de8ac61b",
    "core/omp_wrapper.hpp": "de08b18f7015b0970588f8a3c05b2c8322ed90e0469f813b161c8424ab314116",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    if not args.source:
        parser.exit(2, "β3a native fold requires CUVS_BETA3A_SDK_SOURCE (cuVS cpp/src)\n")
    root = Path(args.source)
    for rel, expected in EXPECTED.items():
        source = root / rel
        try:
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
        except OSError as error:
            parser.exit(2, f"β3a SDK source: {error}\n")
        if digest != expected:
            parser.exit(2, f"β3a SDK source checksum mismatch: {source}\n")


if __name__ == "__main__":
    main()
