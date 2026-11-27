#!/usr/bin/env python3
"""RED controls for src/cuvs_cover_format.h: each mutant weakens one rule of the classification or the
layout; test_cover_format.c must fail (compile error or failed check) for every one of them.
The unmutated header must pass, otherwise a failing mutant would prove nothing."""
import argparse
import json
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

# name -> (exact text in the header, replacement). Each text occurs exactly once.
MUTANTS = {
    "witness-ignored": (
        "if (cuvs_cover_get16(head, special + 4u) == CUVS_COVER_HEAD_WITNESS) {",
        "if (0) {"),
    "legacy-too-broad": (
        "if (upper == special || upper == special - CUVS_COVER_CONN_SIZE) {",
        "if (upper >= special - CUVS_COVER_CONN_SIZE) {"),
    "legacy-when-no-descriptor-magic": (
        "if (upper == special || upper == special - CUVS_COVER_CONN_SIZE) {",
        "if (cuvs_cover_get32(meta, upper) != CUVS_COVER_D_MAGIC) {"),
    "crc-not-checked": (
        "if (stored_crc != want_crc) {",
        "if (0) {"),
    "crc-over-wrong-length": (
        "want_crc = cuvs_cover_crc32c(meta + d_at, d_len - 4u);",
        "want_crc = cuvs_cover_crc32c(meta + d_at, d_len);"),
    "fixed-offset-not-pd_upper": (
        "d_at = upper;",
        "d_at = special - CUVS_COVER_CONN_SIZE - CUVS_COVER_D_SIZE;"),
    "sanity-pd_special-removed": (
        "if (cuvs_cover_get16(meta, CUVS_COVER_OFF_SPECIAL) != special)\n\t\treturn;",
        ""),
    "stage-forgets-pd_upper": (
        "cuvs_cover_put16(page, CUVS_COVER_OFF_UPPER, (uint16_t) d_at);",
        ""),
    "descriptor-128-bytes": (
        "#define CUVS_COVER_D_SIZE 184u",
        "#define CUVS_COVER_D_SIZE 128u"),
    "min-reader-ignored": (
        "if (d.min_reader_version > CUVS_COVER_FORMAT_VERSION) {",
        "if (0) {"),
    "lost-reported-as-certified": (
        "case CUVS_COVER_STATE_LOST:\n\t\tout->cls = CUVS_COVER_LOST;",
        "case CUVS_COVER_STATE_LOST:\n\t\tout->cls = CUVS_COVER_CERTIFIED;"),
    "certified-without-generation": (
        "if (d.generation == 0 || d.ledger_head == CUVS_COVER_INVALID_BLOCK) {",
        "if (d.ledger_head == CUVS_COVER_INVALID_BLOCK) {"),
    "unreadable-head-is-legacy": (
        "if (head == NULL || cuvs_cover_get16(head, CUVS_COVER_OFF_SPECIAL) != special) {\n\t\t\tcuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_HEAD_UNREADABLE);\n\t\t\treturn;\n\t\t}",
        "if (head == NULL) {\n\t\t\tout->cls = CUVS_COVER_LEGACY;\n\t\t\treturn;\n\t\t}"),
    "bad-descriptor-is-legacy": (
        "if (cuvs_cover_get32(meta, d_at) != CUVS_COVER_D_MAGIC) {\n\t\tcuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);\n\t\treturn;\n\t}",
        "if (cuvs_cover_get32(meta, d_at) != CUVS_COVER_D_MAGIC) {\n\t\tout->cls = CUVS_COVER_LEGACY;\n\t\treturn;\n\t}"),    "d_len-minimum-removed": (
        "if (d_len < CUVS_COVER_D_SIZE || (d_len & 7u) != 0 ||",
        "if ((d_len & 7u) != 0 ||"),
    "d_len-alignment-removed": (
        "(d_len & 7u) != 0 || d_at + d_len != special - CUVS_COVER_CONN_SIZE) {",
        "d_at + d_len != special - CUVS_COVER_CONN_SIZE) {"),
    "d_len-end-position-removed": (
        "(d_len & 7u) != 0 || d_at + d_len != special - CUVS_COVER_CONN_SIZE) {",
        "(d_len & 7u) != 0) {"),
    "descriptor-magic-check-removed": (
        "if (cuvs_cover_get32(meta, d_at) != CUVS_COVER_D_MAGIC) {\n\t\tcuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);",
        "if (0) {\n\t\tcuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);"),
    "pd_upper-above-special-allowed": (
        "lower > upper || upper > special)",
        "lower > upper)"),
    "descriptor-head-room-removed": (
        "if (d_at + 16u > special - CUVS_COVER_CONN_SIZE) {",
        "if (0) {"),
    "pd_lower-below-meta-allowed": (
        "if (lower < CUVS_COVER_PGV_META_END || lower > upper",
        "if (lower > upper"),
    "stage-conn-null-not-zeroed": (
        "\telse\n\t\tmemset(page + special - CUVS_COVER_CONN_SIZE, 0, CUVS_COVER_CONN_SIZE);\n",
        "\n"),
    "stage-pd_lower-check-removed": (
        "    cuvs_cover_get16(page, CUVS_COVER_OFF_LOWER) > d_at ||\n",
        ""),
    "stage-d_len-check-removed": (
        "\t    d->d_len != CUVS_COVER_D_SIZE)\n\t\treturn -1;",
        "\t    0)\n\t\treturn -1;"),
    "stage-pd_special-check-removed": (
        "if (cuvs_cover_get16(page, CUVS_COVER_OFF_SPECIAL) != special ||\n",
        "if (0 ||\n"),
    "writable-ignores-format-version": (
        "out->writable = d.format_version <= CUVS_COVER_FORMAT_VERSION;",
        "out->writable = 1;"),
    "writable-never-set": (
        "out->writable = d.format_version <= CUVS_COVER_FORMAT_VERSION;",
        "out->writable = 0;"),
    "untrusted-keeps-writable": (
        "out->reason = why;\n\tout->writable = 0;\n",
        "out->reason = why;\n"),
    "suspect-certified-accepted": (
        "if ((d.flags & CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER) != 0) {",
        "if (0) {"),
    "suspect-reported-as-corrupt": (
        "cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_INCONSISTENT);",
        "cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);"),
}


SANITIZE = ["-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def build(include_dir, test_c, output, flags=()):
    return subprocess.run(
        ["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", *flags, "-I", str(include_dir), str(test_c), "-o", str(output)],
        text=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true",
                        help="build every mutant with ASan/UBSan (about four times slower; the plain run already kills them all)")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    header = (repo / "src/cuvs_cover_format.h").read_text()
    test_c = repo / "test/unit/test_cover_format.c"
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"scope": "cuvs_cover_format.h mutants against test_cover_format.c", "mutants": [], "verdict": "PASS"}
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        (work / "cuvs_cover_format.h").write_text(header)
        # With --sanitize an out-of-bounds read that happens to return the right bytes also kills a mutant.
        flags = SANITIZE if args.sanitize else []
        build_ok = build(work, test_c, work / "base", flags)
        result["sanitizers"] = bool(flags)
        base = subprocess.run([str(work / "base")], capture_output=True, text=True) if build_ok.returncode == 0 else None
        if build_ok.returncode != 0 or base.returncode != 0:
            result["verdict"] = "FAIL"
            result["error"] = "the unmutated header does not pass its own test"
        else:
            for name, (old, new) in MUTANTS.items():
                if header.count(old) != 1:
                    result["verdict"] = "FAIL"
                    result["mutants"].append({"name": name, "status": "NOT-APPLICABLE", "count": header.count(old)})
                    continue
                (work / "cuvs_cover_format.h").write_text(header.replace(old, new, 1))
                built = build(work, test_c, work / name, flags)
                if built.returncode != 0:
                    status = "KILLED(compile)"
                else:
                    # a mutant that reads out of range ends by SIGSEGV; that is a kill, and it should not leave a core file in the working directory
                    ran = subprocess.run([str(work / name)], capture_output=True, text=True, cwd=str(work), preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
                    status = "KILLED(test)" if ran.returncode != 0 else "SURVIVED"
                if status == "SURVIVED":
                    result["verdict"] = "FAIL"
                result["mutants"].append({"name": name, "status": status})
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
