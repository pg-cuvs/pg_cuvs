#!/usr/bin/env python3
"""Source scan for the I3-E SQL surface and docs (no PG, no daemon). CK-26/27
for the I3-E fields.

Asserts, against both 0.9.0 scripts, src/cuvs_ipc.h, src/cuvs_status_text.h,
src/pg_cuvs.c and docs/reference.md:

  * the "-- I3-E begin/end" block is appended after the I3-B block, is the
    same in pg_cuvs--0.9.0.sql and pg_cuvs--0.8.2--0.9.0.sql, drops and
    recreates exactly pg_cuvs_index_status() and pg_cuvs_memory_status(), and
    creates pg_cuvs_build_status();
  * the recreated RETURNS TABLE equals the I3-B header + kinds followed by the
    I3-E header + kinds (names and SQL types), and the I3-B block's columns are
    an unchanged prefix of it;
  * grants: memory_status REVOKEd from PUBLIC and granted to pg_monitor only;
    index_status and build_status keep EXECUTE for PUBLIC;
  * pg_cuvs_build_status in C has as many value assignments as its RETURNS
    TABLE has columns (CUVS_BUILD_STATUS_NATTS) and skips rows that
    cuvs_status_row_visible rejects;
  * docs § 8.4 names every I3-E column of the three functions, and the
    pg_cuvs_build_status row lists its columns in RETURNS TABLE order.

    python3 -I test/unit/check_i3e_sql.py [ROOT]
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


def read(rel):
    with open(os.path.join(ROOT, rel)) as f:
        return f.read()


def block(text, pr):
    m = re.search(rf"^-- {pr} begin\n(.*?)^-- {pr} end\n?", text, flags=re.S | re.M)
    return m


def macro_string(text, name):
    mm = re.search(r"#define\s+" + name + r"\s+((?:\\\n|[^\n])*)", text)
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', mm.group(1))) if mm else None


def macro_int(text, name):
    mm = re.search(r"#define\s+" + name + r"\s+(\d+)", text)
    return int(mm.group(1)) if mm else None


SQL_TYPE = {"o": "oid", "i": "integer", "g": "integer", "l": "bigint", "n": "bigint",
            "t": "timestamptz", "b": "boolean", "r": "boolean", "s": "text"}


def returns_table(blk, fn):
    rm = re.search(r"CREATE FUNCTION\s+" + fn + r"\s*\(\s*\)\s*RETURNS TABLE\s*\((.*?)\)\s*AS",
                   blk, flags=re.S)
    if not rm:
        return None
    return [tuple(c.split()) for c in [x.strip() for x in rm.group(1).split(",")] if c]


up = read("sql/pg_cuvs--0.8.2--0.9.0.sql")
inst = read("sql/pg_cuvs--0.9.0.sql")
mu, mi = block(up, "I3-E"), block(inst, "I3-E")
check(mu is not None and mi is not None, "I3-E begin/end block missing in a 0.9.0 script")
blk = mu.group(1) if mu else ""
check(mu is not None and mi is not None and mu.group(0) == mi.group(0),
      "I3-E block differs between pg_cuvs--0.9.0.sql and pg_cuvs--0.8.2--0.9.0.sql")
for name, text, m in (("upgrade", up, mu), ("install", inst, mi)):
    b = text.find("-- I3-B end")
    check(m is not None and 0 <= b < m.start(), f"{name}: I3-E block is not after the I3-B block")

drops = re.findall(r"DROP FUNCTION\s+(\w+)\s*\(\s*\)\s*;", blk)
check(sorted(drops) == ["pg_cuvs_index_status", "pg_cuvs_memory_status"],
      f"I3-E drops {drops}, want index_status and memory_status")
creates = re.findall(r"CREATE FUNCTION\s+(\w+)\s*\(", blk)
check(sorted(creates) == ["pg_cuvs_build_status", "pg_cuvs_index_status", "pg_cuvs_memory_status"],
      f"I3-E creates {creates}")
for fn in creates:
    check(re.search(rf"COMMENT ON FUNCTION\s+{fn}\s*\(\s*\)\s+IS", blk) is not None,
          f"{fn}: no COMMENT in the I3-E block")
    cm = re.search(rf"CREATE FUNCTION\s+{fn}\s*\(.*?AS\s+'MODULE_PATHNAME'\s*,\s*'(\w+)'\s*"
                   rf"LANGUAGE C VOLATILE\s*;", blk, flags=re.S)
    check(cm is not None and cm.group(1) == fn, f"{fn}: not AS 'MODULE_PATHNAME', '{fn}' LANGUAGE C VOLATILE")

revoked = re.findall(r"REVOKE ALL ON FUNCTION\s+(\w+)\s*\(\s*\)\s+FROM PUBLIC\s*;", blk)
check(revoked == ["pg_cuvs_memory_status"], f"I3-E REVOKEs {revoked}, want memory_status only")
grants = re.findall(r"GRANT\s+(\w+)\s+ON FUNCTION\s+(\w+)\s*\(\s*\)\s+TO\s+(\w+)\s*;", blk)
check(grants == [("EXECUTE", "pg_cuvs_memory_status", "pg_monitor")],
      f"I3-E GRANTs {grants}, want memory_status -> pg_monitor only")

ipc = read("src/cuvs_ipc.h")
kinds_h = read("src/cuvs_status_text.h")
i3b = block(up, "I3-B")
i3b_blk = i3b.group(1) if i3b else ""
for fn, cols, kinds, lead in (
        ("pg_cuvs_index_status", "CUVS_INDEX_STATUS_COLS", "CUVS_INDEX_STATUS_KINDS",
         [("index_name", "text")]),
        ("pg_cuvs_memory_status", "CUVS_MEMORY_STATUS_COLS", "CUVS_MEMORY_STATUS_KINDS", [])):
    h_b = macro_string(ipc, cols).replace("\\t", "\t").split("\t")
    h_e = macro_string(ipc, cols + "_I3E").replace("\\t", "\t").split("\t")
    k_b, k_e = macro_string(kinds_h, kinds), macro_string(kinds_h, kinds + "_I3E")
    check(h_e[0] == "", f"{cols}_I3E does not start with a tab")
    h_e = h_e[1:]
    n_e = macro_int(ipc, cols.replace("_COLS", "_NCOLS") + "_I3E")
    check(len(h_e) == n_e == len(k_e or ""),
          f"{fn}: I3-E header {len(h_e)}, NCOLS_I3E {n_e}, kinds {len(k_e or '')} disagree")
    want = lead + [(n, SQL_TYPE[k]) for n, k in zip(h_b + h_e, k_b + k_e)]
    got = returns_table(blk, fn)
    check(got == want, f"{fn} I3-E RETURNS TABLE differs from joined header/kinds:\n"
                       f"  got  {got}\n  want {want}")
    old = returns_table(i3b_blk, fn)
    check(old is not None and got is not None and got[:len(old)] == old,
          f"{fn}: I3-B columns are not an unchanged prefix of the I3-E definition")

# pg_cuvs_build_status: C body vs RETURNS TABLE.
c_src = read("src/pg_cuvs.c")
bm = re.search(r"^pg_cuvs_build_status\(PG_FUNCTION_ARGS\)\n\{(.*?)^\}", c_src, flags=re.S | re.M)
check(bm is not None, "C function pg_cuvs_build_status not found")
body = bm.group(1) if bm else ""
natts = macro_int(c_src, "CUVS_BUILD_STATUS_NATTS")
bs_cols = returns_table(blk, "pg_cuvs_build_status") or []
check(natts == len(bs_cols), f"CUVS_BUILD_STATUS_NATTS {natts} != {len(bs_cols)} SQL columns")
check(len(re.findall(r"values\[c\+\+\]\s*=", body)) == len(bs_cols),
      "pg_cuvs_build_status assigns a different number of values than its SQL columns")
check(re.search(r"if\s*\(\s*!\s*cuvs_status_row_visible\s*\([^;]*\)\s*\)\s*continue\s*;",
                body, flags=re.S) is not None,
      "pg_cuvs_build_status does not skip rows cuvs_status_row_visible rejects")
check("cuvs_status_is_priv()" in body, "pg_cuvs_build_status does not evaluate the caller's privilege")

# Docs § 8.4.
doc = read("docs/reference.md")
sm = re.search(r"^### 8\.4 .*?(?=^### 8\.5 )", doc, flags=re.S | re.M)
sec = sm.group(0) if sm else ""
check(sm is not None, "docs § 8.4 missing")
for cols in ("CUVS_MEMORY_STATUS_COLS_I3E", "CUVS_INDEX_STATUS_COLS_I3E"):
    for name in macro_string(ipc, cols).replace("\\t", "\t").split("\t")[1:]:
        if re.match(r"rejects_ar[1-6]$", name):
            continue          # written as the range `rejects_ar0` … `rejects_ar7`
        check(f"`{name}`" in sec, f"docs § 8.4 does not name `{name}`")
row = [l for l in sec.splitlines() if l.startswith("| `pg_cuvs_build_status()`")]
check(len(row) == 1, "docs § 8.4 has no single pg_cuvs_build_status() row")
if row:
    named = re.findall(r"`(\w+)`", row[0].split("|")[3])
    order = [n for n in named if n in {c[0] for c in bs_cols}]
    seen = []
    for n in order:
        if n not in seen:
            seen.append(n)
    check(seen == [c[0] for c in bs_cols],
          f"docs build_status row column order {seen} != {[c[0] for c in bs_cols]}")

if FAILS:
    for f in FAILS:
        print("FAIL", f)
    print(f"check_i3e_sql: {len(FAILS)} failure(s)")
    sys.exit(1)
print("check_i3e_sql: OK (I3-E block in both scripts, joined RETURNS TABLE, grants, "
      "build_status C/SQL arity and row filter, docs § 8.4 columns)")
