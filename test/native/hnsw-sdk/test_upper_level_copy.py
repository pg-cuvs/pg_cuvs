#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
"""Compile the exact cuVS disk-serializer copy statement against fractional floats."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile
import textwrap


EXPECTED_SOURCE_COMMIT = "60b19a0f62ba0bd716de349382828e43be6e2daa"
SOURCE_FILE = pathlib.Path("cpp/src/neighbors/detail/hnsw.hpp")
DISK_COPY_BLOCK = re.compile(
    r"if\s*\(create_hierarchy\s*&&\s*levels\[i\]\s*>\s*0\)\s*\{(?P<body>.*?)^\s*\}",
    re.MULTILINE | re.DOTALL,
)
COPY_STATEMENT = re.compile(r"std::copy\s*\([^;]*;", re.DOTALL)


def one_copy_statement(text: str, context: str) -> str:
    statements = list(COPY_STATEMENT.finditer(text))
    if len(statements) != 1:
        raise SystemExit(f"expected one std::copy in {context}, found {len(statements)}")
    return statements[0].group(0)


def source_copy_statements(source_root: pathlib.Path) -> tuple[str, str]:
    commit = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if commit != EXPECTED_SOURCE_COMMIT:
        raise SystemExit(
            f"source commit mismatch: expected {EXPECTED_SOURCE_COMMIT}, got {commit}"
        )

    source_text = (source_root / SOURCE_FILE).read_text(encoding="utf-8")
    disk_blocks = list(DISK_COPY_BLOCK.finditer(source_text))
    if len(disk_blocks) != 1:
        raise SystemExit(f"expected one disk upper-level hierarchy block, found {len(disk_blocks)}")

    memory_start = source_text.index("// gather points from dataset to form query set on host")
    memory_end = source_text.index("// find neighbors of the query set", memory_start)
    memory_region = source_text[memory_start:memory_end]
    return (
        one_copy_statement(disk_blocks[0].group("body"), "disk serializer hierarchy block"),
        one_copy_statement(memory_region, "in-memory GPU hierarchy gather"),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--compiler", default="clang++")
    args = parser.parse_args()

    disk_statement, memory_statement = source_copy_statements(args.source.resolve())
    harness = textwrap.dedent(
        """
        #include <cstddef>
        #include <cstring>
        #include <iostream>

        struct HostMatrix {
          float* values;
          float& operator()(std::size_t row, std::size_t col) noexcept
          {
            return values[row * 4 + col];
          }
        };

        struct FakeHnsw {
          const float* values;
          const float* getDataByInternalId(std::size_t id) const noexcept
          {
            return values + id * 4;
          }
        };

        int main()
        {
          constexpr std::size_t dim = 4;
          const float original[dim] = {0.5F, -1.75F, 2.25F, -3.5F};
          bool passed = true;
          {
            float copied[dim] = {};
            const float* data_row = original;
            const std::size_t i = 0;
            const std::size_t order_bw[1] = {0};
            const std::size_t hist[1] = {0};
            HostMatrix host_query_set{copied};

            // Inserted verbatim from the pinned disk serializer hierarchy block.
            __DISK_SOURCE_COPY_STATEMENT__
            if (std::memcmp(original, copied, sizeof(original)) != 0) {
              std::cerr << "FAIL: disk serializer hierarchy copy changed float bytes\\n";
              passed = false;
            } else {
              std::cout << "PASS: disk serializer hierarchy copy preserves float bytes\\n";
            }
          }
          {
            float copied[dim] = {};
            FakeHnsw fake_hnsw{original};
            FakeHnsw* appr_algo = &fake_hnsw;
            const std::size_t pt_id = 0;
            const std::size_t i = 0;
            const std::size_t start_idx = 0;
            HostMatrix host_query_set{copied};

            // Inserted verbatim from the pinned in-memory GPU hierarchy gather.
            __MEMORY_SOURCE_COPY_STATEMENT__
            if (std::memcmp(original, copied, sizeof(original)) != 0) {
              std::cerr << "FAIL: in-memory GPU hierarchy copy changed float bytes\\n";
              passed = false;
            } else {
              std::cout << "PASS: in-memory GPU hierarchy copy preserves float bytes\\n";
            }
          }
          return passed ? 0 : 1;
        }
        """
    ).replace("__DISK_SOURCE_COPY_STATEMENT__", disk_statement).replace(
        "__MEMORY_SOURCE_COPY_STATEMENT__", memory_statement
    )

    with tempfile.TemporaryDirectory(prefix="cuvs-hnsw-copy-") as temp_dir:
        temp_path = pathlib.Path(temp_dir)
        cpp_file = temp_path / "upper_level_copy.cpp"
        binary = temp_path / "upper_level_copy"
        cpp_file.write_text(harness, encoding="utf-8")
        compile_result = subprocess.run(
            [args.compiler, "-std=c++20", "-O0", str(cpp_file), "-o", str(binary)],
            check=False,
        )
        if compile_result.returncode != 0:
            return compile_result.returncode
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
