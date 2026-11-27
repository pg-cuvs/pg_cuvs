#!/usr/bin/env python3
"""RED controls for src/cuvs_cover_ledger.h: each mutant removes or weakens one rule of the ledger page code;
test_cover_ledger.c (built with ASan/UBSan) must fail, crash or time out for every one of them. The unmutated
header must pass, otherwise a failing mutant proves nothing."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SANITIZE = ["-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]

# name -> (exact text in the header, replacement). Each text occurs exactly once. Left out as equivalent: the pd_lower
# range test and the count cap, the `i >= h.count` test of cuvs_ledger_key_at (a slot past the count is zeros, whose crc never
# matches), because `lower == KEYS_AT + 24 * count` together with either one bounds every read. Equivalent mutants are left out on
# purpose: the `base[blk] == NO_BLOCK` test in cuvs_reach_slot is implied by width[blk] == 0 for blocks outside the chain,
# and a relation of one block is refused as NOT_HNSW by the empty chain anyway; both guards stay as defence in depth.
MUTANTS = {
    "page-id-not-checked": ("|| cuvs_cover_get16(page, special + 6u) != CUVS_LEDGER_PAGE_ID)", ")"),
    "special-offset-not-checked": ("if (cuvs_cover_get16(page, CUVS_COVER_OFF_SPECIAL) != special ||", "if (0 ||"),
    "pd_upper-not-checked": ("if (upper != special || lower < CUVS_LEDGER_KEYS_AT || lower > special)", "if (lower < CUVS_LEDGER_KEYS_AT || lower > special)"),
    "magic-not-checked": ("if (h.magic != CUVS_LEDGER_MAGIC || h.version == 0", "if (h.version == 0"),
    "version-not-checked": ("|| h.version == 0 || h.version > CUVS_LEDGER_VERSION ||", "||"),
    "kind-not-checked": ("(h.kind != CUVS_LEDGER_KIND_KEYS && h.kind != CUVS_LEDGER_KIND_BASE) ||", ""),
    "header-crc-not-checked": ("h.crc32c != cuvs_ledger_header_crc(&h) ||", ""),
    "special-kind-mirror-not-checked": ("cuvs_cover_get16(page, special + 4u) != h.kind)", "0)"),
    "pd_lower-vs-count-not-checked": ("if (lower != CUVS_LEDGER_KEYS_AT + CUVS_LEDGER_KEY_SIZE * h.count)", "if (0)"),
    "header-write-forgets-pd_lower": ("	cuvs_cover_put16(page, CUVS_COVER_OFF_LOWER, (uint16_t) (CUVS_LEDGER_KEYS_AT + CUVS_LEDGER_KEY_SIZE * w.count));\n", ""),
    "drop-listed-keeps-the-listed": ("		if (!found)\n			keys[w++] = keys[i];", "		keys[w++] = keys[i];"),
    "drop-listed-matches-the-block-only": ("	return ((uint64_t) block << 16) | offset;", "	return ((uint64_t) block << 16);"),
    "drop-listed-search-stops-early": ("			if (listed[mid] < code)\n				lo = mid + 1;", "			if (listed[mid] < code)\n				lo = hi;"),
    "carry-advances-by-the-kept": ("	*advance = n;\n	return cuvs_ledger_drop_listed(keys, n, listed, nlisted);", "	size_t kept = cuvs_ledger_drop_listed(keys, n, listed, nlisted);\n	*advance = kept;\n	return kept;"),
    "raise-seen-forgets-the-pages": ("		h.graph_nblocks_seen += pages;", "		h.graph_nblocks_seen += 0;"),
    "raise-seen-raises-an-unknown-size": ("	if (h.graph_nblocks_seen != 0)\n	{\n		h.graph_nblocks_seen += pages;", "	if (1)\n	{\n		h.graph_nblocks_seen += pages;"),
    "raise-seen-does-not-write": ("		h.graph_nblocks_seen += pages;\n		cuvs_ledger_write_header(page, &h);", "		h.graph_nblocks_seen += pages;"),
    "append-forgets-seen": ("	h.count++;\n	h.graph_nblocks_seen = seen;", "	h.count++;"),
    "crc-excludes-seen": ("return cuvs_cover_crc32c(h, offsetof(CuvsLedgerHeader, crc32c));", "return cuvs_cover_crc32c(h, offsetof(CuvsLedgerHeader, graph_nblocks_seen));"),
    "init-forgets-seen": ("	h.graph_nblocks_seen = seen;\n	cuvs_ledger_write_header(page, &h);\n}\n\n/* Check a page", "	cuvs_ledger_write_header(page, &h);\n}\n\n/* Check a page"),
    "append-full-not-checked": ("if (h.count >= CUVS_LEDGER_CAPACITY)", "if (0)"),
    "append-ordinal-not-checked": ("if (k->ordinal != h.first_ordinal + h.count || k->ordinal > CUVS_LEDGER_ORDINAL_MASK)", "if (k->ordinal > CUVS_LEDGER_ORDINAL_MASK)"),
    "append-validates-nothing": ("	CuvsLedgerStatus st = cuvs_ledger_validate(page, &h);\n\n	if (st != CUVS_LEDGER_OK)\n		return st;\n	if (h.count >= CUVS_LEDGER_CAPACITY)", "	CuvsLedgerStatus st = CUVS_LEDGER_OK;\n	memcpy(&h, page + CUVS_COVER_PAGE_HEADER, sizeof(h));\n	if (h.count >= CUVS_LEDGER_CAPACITY)"),
    "walk-length-not-bounded": ("if (pages++ >= max_pages)", "if (0)"),
    "walk-generation-not-checked": ("if (h.generation != generation)", "if (0)"),
    "walk-gap-not-checked": ("if (h.first_ordinal != next_ordinal)", "if (0)"),
    "walk-key-ordinal-not-checked": ("if (k.ordinal != h.first_ordinal + i)", "if (0)"),
    "walk-ignores-invalid-page": ("if (cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK)\n			return CUVS_LEDGER_WALK_INVALID;", "(void) cuvs_ledger_validate(page, &h);"),
    "key-crc-not-written": ("	w0 |= (uint64_t) crc << 48;\n	memcpy(dst, &w0, 8);", "	memcpy(dst, &w0, 8);"),
    "key-crc-not-checked": ("	if (cuvs_ledger_crc16(b, sizeof(b)) != crc)\n		return 1;", "	(void) crc;"),
    "key-crc-covers-less": ("	crc = cuvs_ledger_crc16(dst, CUVS_LEDGER_KEY_SIZE);", "	crc = cuvs_ledger_crc16(dst, 16);"),
    "key-ordinal-not-masked": ("	uint64_t w0 = k->ordinal & CUVS_LEDGER_ORDINAL_MASK;", "	uint64_t w0 = k->ordinal;"),
    "ordinal-limit-not-checked": ("	if (k->ordinal != h.first_ordinal + h.count || k->ordinal > CUVS_LEDGER_ORDINAL_MASK)", "	if (k->ordinal != h.first_ordinal + h.count)"),
    "walk-bad-key-ignored": ("			if (cuvs_ledger_key_decode(page + CUVS_LEDGER_KEYS_AT + (size_t) CUVS_LEDGER_KEY_SIZE * i, &k) != 0)\n				return CUVS_LEDGER_WALK_BAD_KEY;", "			(void) cuvs_ledger_key_decode(page + CUVS_LEDGER_KEYS_AT + (size_t) CUVS_LEDGER_KEY_SIZE * i, &k);"),
    "walk-tail-not-required": ("	return included ? CUVS_LEDGER_WALK_OK : CUVS_LEDGER_WALK_TAIL_MISSING;", "	return CUVS_LEDGER_WALK_OK;"),
    "walk-tail-never-seen": ("		if (blk == must_include)\n			included = 1;", "		if (blk == must_include + 1u)\n			included = 1;"),
}


def build(include_dir, test_c, output):
    return subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", *SANITIZE, "-I", str(include_dir), str(test_c), "-o", str(output)],
                          text=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    header = (repo / "src/cuvs_cover_ledger.h").read_text()
    test_c = repo / "test/unit/test_cover_ledger.c"
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"scope": "cuvs_cover_ledger.h mutants against test_cover_ledger.c (ASan/UBSan)", "mutants": [], "verdict": "PASS"}
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        (work / "cuvs_cover_ledger.h").write_text(header)
        (work / "cuvs_cover_format.h").write_text((repo / "src/cuvs_cover_format.h").read_text())
        built = build(work, test_c, work / "base")
        base = subprocess.run([str(work / "base")], capture_output=True, text=True, timeout=120) if built.returncode == 0 else None
        if built.returncode != 0 or base.returncode != 0:
            result.update(verdict="FAIL", error="the unmutated header does not pass its own test: " + (built.stderr or base.stderr)[-300:])
        else:
            for name, (old, new) in MUTANTS.items():
                if header.count(old) != 1:
                    result["verdict"] = "FAIL"
                    result["mutants"].append({"name": name, "status": "NOT-APPLICABLE", "count": header.count(old)})
                    continue
                (work / "cuvs_cover_ledger.h").write_text(header.replace(old, new, 1))
                b = build(work, test_c, work / name)
                if b.returncode != 0:
                    status = "KILLED(compile)"
                else:
                    try:
                        ran = subprocess.run([str(work / name)], capture_output=True, text=True, timeout=60)
                        status = "KILLED(test)" if ran.returncode != 0 else "SURVIVED"
                    except subprocess.TimeoutExpired:
                        status = "KILLED(timeout)"
                if status == "SURVIVED":
                    result["verdict"] = "FAIL"
                result["mutants"].append({"name": name, "status": status})
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
