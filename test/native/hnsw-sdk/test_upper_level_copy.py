#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
"""Compile both exact copy expressions with the real hnswlib getter type."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap


EXPECTED_SOURCE_COMMIT = "60b19a0f62ba0bd716de349382828e43be6e2daa"
SOURCE_FILE = pathlib.Path("cpp/src/neighbors/detail/hnsw.hpp")
DISK_COPY_BLOCK = re.compile(
    r"if\s*\(create_hierarchy\s*&&\s*levels\[i\]\s*>\s*0\)\s*\{(?P<body>.*?)^\s*\}",
    re.MULTILINE | re.DOTALL,
)
COPY_STATEMENT = re.compile(r"std::copy\s*\([^;]*;", re.DOTALL)
COPY_MISMATCH_EXIT = 1
SETUP_FAILURE_EXIT = 2
COMPILE_FAILURE_EXIT = 3


class CopySetupError(RuntimeError):
    pass


def one_copy_statement(text: str, context: str) -> str:
    statements = list(COPY_STATEMENT.finditer(text))
    if len(statements) != 1:
        raise CopySetupError(f"expected one std::copy in {context}, found {len(statements)}")
    return statements[0].group(0)


def source_copy_statements(source_root: pathlib.Path) -> tuple[str, str]:
    commit = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if commit != EXPECTED_SOURCE_COMMIT:
        raise CopySetupError(
            f"source commit mismatch: expected {EXPECTED_SOURCE_COMMIT}, got {commit}"
        )

    source_text = (source_root / SOURCE_FILE).read_text(encoding="utf-8")
    disk_blocks = list(DISK_COPY_BLOCK.finditer(source_text))
    if len(disk_blocks) != 1:
        raise CopySetupError(f"expected one disk upper-level hierarchy block, found {len(disk_blocks)}")

    memory_start = source_text.index("// gather points from dataset to form query set on host")
    memory_end = source_text.index("// find neighbors of the query set", memory_start)
    memory_region = source_text[memory_start:memory_end]
    return (
        one_copy_statement(disk_blocks[0].group("body"), "disk serializer hierarchy block"),
        one_copy_statement(memory_region, "in-memory GPU hierarchy gather"),
    )


def prepare(args) -> tuple[str, str]:
    disk_statement, memory_statement = source_copy_statements(args.source.resolve())
    getter = args.hnswlib_header.read_text(encoding="utf-8")
    if not re.search(r"char\s*\*\s*getDataByInternalId\s*\(", getter):
        raise CopySetupError("actual hnswlib getter must return char*; update the fixture if its API changes")
    if args.mutant == "raw-source":
        memory_statement, count = re.subn(
            r"reinterpret_cast<const T\*>\((appr_algo->getDataByInternalId\(pt_id\))\)",
            r"\1", memory_statement,
        )
        if count != 2:
            raise CopySetupError("raw-source mutant must replace both typed iterators")
    elif args.mutant == "byte-destination":
        if memory_statement.count("&host_query_set(i - start_idx, 0)") != 1:
            raise CopySetupError("byte-destination mutant must replace one destination")
        memory_statement = memory_statement.replace(
            "&host_query_set(i - start_idx, 0)",
            "reinterpret_cast<char*>(&host_query_set(i - start_idx, 0))",
        )
    elif args.mutant == "short-copy":
        if memory_statement.count("+ dim") != 1:
            raise CopySetupError("short-copy mutant must replace one element count")
        memory_statement = memory_statement.replace("+ dim", "+ dim / sizeof(T)")
    return disk_statement, memory_statement


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--hnswlib-header", required=True, type=pathlib.Path)
    parser.add_argument("--mutant", choices=["raw-source", "byte-destination", "short-copy"])
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    result = {"phase": "setup", "mutant": args.mutant,
              "compile_exit": None, "runtime_exit": None}

    def finish(exit_code: int) -> int:
        if args.output:
            args.output.write_text(json.dumps(result, indent=2) + "\n")
        if "error" in result:
            print(f"ERROR copy {result['phase']}: {result['error']}", file=sys.stderr)
        return exit_code

    try:
        disk_statement, memory_statement = prepare(args)
    except (CopySetupError, OSError, ValueError, subprocess.CalledProcessError) as exc:
        result["error"] = str(exc)
        return finish(SETUP_FAILURE_EXIT)
    harness = textwrap.dedent(
        """
        #include <algorithm>
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
          char* getDataByInternalId(std::size_t id) const noexcept
          {
            return const_cast<char*>(reinterpret_cast<const char*>(values + id * 4));
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
            using T = float;
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
        result["phase"] = "compile"
        try:
            compile_result = subprocess.run(
                [args.compiler, "-std=c++20", "-O0", str(cpp_file), "-o", str(binary)],
                check=False,
            )
        except OSError as exc:
            result["error"] = str(exc)
            return finish(COMPILE_FAILURE_EXIT)
        result["compile_exit"] = compile_result.returncode
        if compile_result.returncode != 0:
            result["error"] = "copy harness compilation failed"
            return finish(COMPILE_FAILURE_EXIT)
        result["phase"] = "runtime"
        result["runtime_exit"] = subprocess.run([str(binary)], check=False).returncode
        # Only the compiled harness's explicit mismatch exit is a detected mutant.
        return finish(result["runtime_exit"] if result["runtime_exit"] >= 0 else 1)


if __name__ == "__main__":
    raise SystemExit(main())
