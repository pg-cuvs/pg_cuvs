#!/usr/bin/env python3
"""RED controls for src/cuvs_cover_reach.h: each mutant removes or weakens one rule of the reachability walk;
test_cover_reach.c (built with ASan/UBSan) must fail, crash or time out for every one of them. The unmutated
header must pass, otherwise a failing mutant proves nothing."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SANITIZE = ["-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]

# name -> (exact text in the header, replacement). Each text occurs exactly once. Equivalent mutants are left out on
# purpose: the minimum length in cuvs_reach_item (pass A now rejects every item shorter than its type needs before any other
# pass reads it), a chain page that cannot be read ending the chain quietly (the check on the pages the chain skipped reads
# that page again and fails the same way),
# the `base[blk] == NO_BLOCK` test in cuvs_reach_slot (implied by width[blk] == 0 outside the chain), the
# emit pass ignoring a read failure by `break` (the chain-length check then reports the damage), a relation of one
# block (refused as NOT_HNSW by the empty chain anyway). Equivalent mutants are left out on
# purpose: the `base[blk] == NO_BLOCK` test in cuvs_reach_slot is implied by width[blk] == 0 for blocks outside the chain,
# and a relation of one block is refused as NOT_HNSW by the empty chain anyway; both guards stay as defence in depth.
MUTANTS = {
    "slot-block-bound-removed": ("if (blk >= s->nblocks || s->base[blk] == CUVS_REACH_NO_BLOCK", "if (s->base[blk] == CUVS_REACH_NO_BLOCK"),
    "slot-width-check-removed": ("|| off < 1 || off > s->width[blk])", "|| off < 1)"),
    "neighbor-live-check-removed": ("if (ts == UINT64_MAX || !cuvs_reach_bit(s.live, ts) || cuvs_reach_bit(s.seen, ts))", "if (ts == UINT64_MAX || cuvs_reach_bit(s.seen, ts))"),
    "neighbor-seen-check-removed": ("if (ts == UINT64_MAX || !cuvs_reach_bit(s.live, ts) || cuvs_reach_bit(s.seen, ts))", "if (ts == UINT64_MAX || !cuvs_reach_bit(s.live, ts))"),
    "item-end-check-removed": ("|| at + n > CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE)", ")"),
    "item-flags-ignored": ("if (flags != 1u || n < minlen ||", "if ((flags & 4u) != 0 || n < minlen ||"),
    "neighbor-count-cap-removed-same-page": ("						if (ntid > (len - CUVS_REACH_NEIGHBOR_FIXED) / CUVS_REACH_TID_SIZE)\n							ntid = (len - CUVS_REACH_NEIGHBOR_FIXED) / CUVS_REACH_TID_SIZE;\n						memcpy(eparts, item, CUVS_REACH_NEIGHBOR_FIXED + (size_t) ntid * CUVS_REACH_TID_SIZE);\n					} else {", "						memcpy(eparts, item, CUVS_REACH_NEIGHBOR_FIXED + (size_t) ntid * CUVS_REACH_TID_SIZE);\n					} else {"),
    "chain-loop-ends-quietly": ("if (s.base[blk] != CUVS_REACH_NO_BLOCK || hops++ > s.nblocks) {	/* loop in the chain */\n			s.fail = CUVS_REACH_DAMAGED;\n			goto done;\n		}", "if (s.base[blk] != CUVS_REACH_NO_BLOCK || hops++ > s.nblocks) {	/* loop in the chain */\n			break;\n		}"),
    "chain-bad-header-ends-quietly": ("		if (maxoff < 0) {\n			s.fail = blk == CUVS_REACH_HEAD_BLKNO ? CUVS_REACH_NOT_HNSW : CUVS_REACH_DAMAGED;\n			goto done;\n		}", "		if (maxoff < 0) {\n			break;\n		}"),
    "link-outside-relation-ends-quietly": ("			if (blk != 0 && blk < src->nblocks(src->ctx))\n				break;\n			s.fail = CUVS_REACH_DAMAGED;\n			goto done;", "			break;"),
    "link-to-metapage-allowed": ("if (blk == 0 || blk >= s.nblocks) {", "if (blk >= s.nblocks) {"),
    "append-after-start-counted-as-damage": ("if (blk != 0 && blk < src->nblocks(src->ctx))", "if (0)"),
    "emit-chain-length-not-checked": ("if (visited != nchain) {", "if (visited != nchain && 0) {"),
    "emit-width-check-removed": ("for (off = 1; off <= (unsigned) maxoff && off <= s.width[blk]; off++) {", "for (off = 1; off <= (unsigned) maxoff; off++) {"),
    "emit-first-tid-only": ("for (t = 0; t < CUVS_REACH_HEAPTIDS; t++) {\n					size_t at = 4u + (size_t) t * CUVS_REACH_TID_SIZE;\n					uint32_t hb = ((uint32_t) cuvs_reach_u16(e, at) << 16) | cuvs_reach_u16(e, at + 2);\n					uint16_t ho = cuvs_reach_u16(e, at + 4);\n\n					if (ho == 0)	/* invalid ItemPointer */", "for (t = 0; t < 1; t++) {\n					size_t at = 4u + (size_t) t * CUVS_REACH_TID_SIZE;\n					uint32_t hb = ((uint32_t) cuvs_reach_u16(e, at) << 16) | cuvs_reach_u16(e, at + 2);\n					uint16_t ho = cuvs_reach_u16(e, at + 4);\n\n					if (ho == 0)	/* invalid ItemPointer */"),
    "emit-invalid-heap-tid": ("if (ho == 0)\t/* invalid ItemPointer */", "if (0)"),
    "emit-reached-too": ("if (!cuvs_reach_bit(s.live, slot) || cuvs_reach_bit(s.seen, slot))", "if (!cuvs_reach_bit(s.live, slot))"),
    "deleted-flag-ignored": ("if (e[2] == 0) {", "if (1) {"),
    "entry-live-check-removed": ("if (entry_slot != UINT64_MAX && cuvs_reach_bit(s.live, entry_slot)) {", "if (entry_slot != UINT64_MAX) {"),
    "neighbor-tuple-block-bound-removed": ("					if (eb >= s.nblocks)\n						continue;", ""),
    "memory-limit-ignored": ("if (s->used + add > s->limit) {", "if (0) {"),
    "metapage-magic-ignored": ("|| cuvs_reach_u32(s.page, 24) != CUVS_REACH_PGV_MAGIC) {", ") {"),
    "page-header-sanity-weakened": ("|| lower > upper || upper > special)", "|| lower > 99999 || upper > 99999)"),
    "neighbor-type-ignored-same-page": ("item = cuvs_reach_item(s.page, maxoff, eo, CUVS_REACH_NEIGHBOR_FIXED, &len);\n						if (item == NULL || item[0] != CUVS_REACH_NEIGHBOR_TYPE)", "item = cuvs_reach_item(s.page, maxoff, eo, CUVS_REACH_NEIGHBOR_FIXED, &len);\n						if (item == NULL)"),
    "neighbor-type-ignored-other-page": ("item = cuvs_reach_item(other, nmaxoff, eo, CUVS_REACH_NEIGHBOR_FIXED, &len);\n						if (item == NULL || item[0] != CUVS_REACH_NEIGHBOR_TYPE)", "item = cuvs_reach_item(other, nmaxoff, eo, CUVS_REACH_NEIGHBOR_FIXED, &len);\n						if (item == NULL)"),
    "element-type-ignored-in-search": ("if (item == NULL || item[0] != CUVS_REACH_ELEMENT_TYPE)\n						continue;\n					/* neighbortid", "if (item == NULL)\n						continue;\n					/* neighbortid"),
    "unreached-not-subtracted": ("out->unreached = out->live - out->reached;", "out->unreached = out->live;"),
    "frontier-not-cleared-between-rounds": ("memset(s.nxt, 0, (size_t) frontier_bytes);", ""),
    "page-read-for-every-element": ("						have_page = 1;", "						have_page = 0;"),
    "frontier-not-swapped": ("				s.cur = s.nxt;\n				s.nxt = t;", "				s.nxt = t;"),
    "entry-not-in-frontier": ("			cuvs_reach_setbit(s.cur, entry_slot);", ""),
    "next-frontier-not-set": ("						cuvs_reach_setbit(s.nxt, ts);", ""),
    "delay-hook-removed-chain-pass": ("\n\t\tif (src->delay != NULL && (pages_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {", "\n\t\tif (0) {"),
    "delay-hook-removed-unvisited-pass": ("(skipped_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {", "(skipped_done++ % CUVS_REACH_DELAY_EVERY) == 0 && 0) {"),
    "delay-hook-removed-search": ("(nodes_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {", "(nodes_done++ % CUVS_REACH_DELAY_EVERY) == 0 && 0) {"),
    "delay-hook-removed-emit-pass": ("\n\t\t\tif (src->delay != NULL && (pages_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {", "\n\t\t\tif (0) {"),
    "ram-accounting-halved": ("\t\ts->used += bytes;\n\t\tif (s->used > s->peak)", "\t\ts->used += bytes / 2u;\n\t\tif (s->used > s->peak)"),
    "spill-never-used": ("} else if (s->src->big_alloc != NULL && s->spilled + bytes <= s->spill_limit &&", "} else if (0 && s->src->big_alloc != NULL && s->spilled + bytes <= s->spill_limit &&"),
    "spill-limit-ignored": ("s->spilled + bytes <= s->spill_limit &&\n		   (p =", "1 &&\n		   (p ="),
    "spill-counter-not-given-back": ("	if (b->spilled)\n		s->spilled -= b->bytes;", "	if (b->spilled)\n		s->spilled -= 0;"),
    "ram-counter-not-given-back": ("	else\n		s->used -= b->bytes;", "	else\n		s->used -= 0;"),
    "needed-bytes-not-reported": ("out->needed_bytes = s.want;", ""),
    "projection-not-reported": ("			if (projected > s->want)\n				s->want = projected;", "			(void) projected;"),
    "bitmap-contents-lost-on-growth": ("		memcpy(nb.p, b->p, b->bytes < bytes_new ? b->bytes : bytes_new);", "		(void) 0;"),
    "page-scratch-not-accounted": ("	if (!cuvs_reach_account(&s, CUVS_REACH_PAGE_SIZE)) {\n		cuvs_reach_project_first(&s);\n		goto done;\n	}", ""),
    "result-kept-on-failure": ("	if (out->status != CUVS_REACH_OK) {\n		out->reached = out->unreached = out->emitted = 0;\n	}", ""),
    "item-malformed-skipped": ("e[0] == CUVS_REACH_NEIGHBOR_TYPE ? len < CUVS_REACH_NEIGHBOR_FIXED : 1)) {\n\t\t\t\ts.fail = CUVS_REACH_DAMAGED;\n\t\t\t\tgoto done;\n\t\t\t}", "e[0] == CUVS_REACH_NEIGHBOR_TYPE ? len < CUVS_REACH_NEIGHBOR_FIXED : 1)) {\n\t\t\t\tcontinue;\n\t\t\t}"),
    "item-unknown-type-allowed": ("e[0] == CUVS_REACH_NEIGHBOR_TYPE ? len < CUVS_REACH_NEIGHBOR_FIXED : 1)) {", "e[0] == CUVS_REACH_NEIGHBOR_TYPE ? len < CUVS_REACH_NEIGHBOR_FIXED : 0)) {"),
    "element-length-unchecked": ("(e[0] == CUVS_REACH_ELEMENT_TYPE ? len < CUVS_REACH_ELEMENT_FIXED :", "(e[0] == CUVS_REACH_ELEMENT_TYPE ? 0 :"),
    "neighbor-length-unchecked": ("e[0] == CUVS_REACH_NEIGHBOR_TYPE ? len < CUVS_REACH_NEIGHBOR_FIXED : 1))", "e[0] == CUVS_REACH_NEIGHBOR_TYPE ? 0 : 1))"),
    "unvisited-pages-not-checked": ("for (b = CUVS_REACH_HEAD_BLKNO + 1u; b < s.nblocks; b++) {", "for (b = s.nblocks; b < s.nblocks; b++) {"),
    "unvisited-page-id-ignored": ("cuvs_reach_u16(s.page, CUVS_REACH_PAGE_ID_AT) == CUVS_REACH_PGV_PAGE_ID &&", "cuvs_reach_u16(s.page, CUVS_REACH_PAGE_ID_AT) == 0xFFFFu &&"),
    "unvisited-empty-graph-page-damaged": ("cuvs_reach_maxoff(s.page) != 0) {", "cuvs_reach_maxoff(s.page) != 12345) {"),
    "unvisited-unreadable-ignored": ("			if (src->read_block(src->ctx, b, s.page) != 0) {\n				s.fail = CUVS_REACH_DAMAGED;\n				goto done;\n			}\n			if (cuvs_reach_u16(s.page, CUVS_REACH_PAGE_ID_AT)", "			if (src->read_block(src->ctx, b, s.page) != 0) {\n				continue;\n			}\n			if (cuvs_reach_u16(s.page, CUVS_REACH_PAGE_ID_AT)"),
    "unvisited-looks-past-the-start-size": ("for (b = CUVS_REACH_HEAD_BLKNO + 1u; b < s.nblocks; b++) {", "for (b = CUVS_REACH_HEAD_BLKNO + 1u; b < src->nblocks(src->ctx); b++) {"),
    "emit-changed-element-skipped": ("				if (e == NULL || e[0] != CUVS_REACH_ELEMENT_TYPE) {\n					/* pass A saw a live element here and tuples are only ever added: this is damage, and skipping it would\n					 * leave an unreached row out of the keys */\n					s.fail = CUVS_REACH_DAMAGED;\n					goto done;\n				}", "				if (e == NULL || e[0] != CUVS_REACH_ELEMENT_TYPE)\n					continue;"),
    "first-projection-dropped-at-scratch": ("	if (!cuvs_reach_account(&s, CUVS_REACH_PAGE_SIZE)) {\n		cuvs_reach_project_first(&s);\n		goto done;\n	}", "	if (!cuvs_reach_account(&s, CUVS_REACH_PAGE_SIZE)) {\n		goto done;\n	}"),
    "first-projection-dropped-at-table": ("	    !cuvs_reach_buf_alloc(&s, &s.b_width, (size_t) s.nblocks * sizeof(uint16_t))) {\n		cuvs_reach_project_first(&s);", "	    !cuvs_reach_buf_alloc(&s, &s.b_width, (size_t) s.nblocks * sizeof(uint16_t))) {"),
    "first-projection-without-bitmaps": ("		4u * (((uint64_t) s->nblocks * CUVS_REACH_TYPICAL_SLOTS + 7u) / 8u);", "		0u;"),
    "first-projection-twice-the-bitmaps": ("		4u * (((uint64_t) s->nblocks * CUVS_REACH_TYPICAL_SLOTS + 7u) / 8u);", "		8u * (((uint64_t) s->nblocks * CUVS_REACH_TYPICAL_SLOTS + 7u) / 8u);"),
}


def build(include_dir, test_c, output):
    return subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", *SANITIZE, "-I", str(include_dir), str(test_c), "-o", str(output)],
                          text=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    header = (repo / "src/cuvs_cover_reach.h").read_text()
    test_c = repo / "test/unit/test_cover_reach.c"
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"scope": "cuvs_cover_reach.h mutants against test_cover_reach.c (ASan/UBSan)", "mutants": [], "verdict": "PASS"}
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        (work / "cuvs_cover_reach.h").write_text(header)
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
                (work / "cuvs_cover_reach.h").write_text(header.replace(old, new, 1))
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
