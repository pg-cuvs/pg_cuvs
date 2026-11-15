#!/usr/bin/env python3
"""Source scan for the I3-B SQL surface (no PG, no daemon).

Asserts, against src/pg_cuvs.c, src/cuvs_ipc.h, src/cuvs_status_text.h and the
I3-B block of sql/pg_cuvs--0.8.2--0.9.0.sql:

  * the superuser-only C entry points (pg_cuvs_reclaim_set, pg_cuvs_unload_oid,
    pg_cuvs_unpin_oid) reach an `if (!superuser())` guard, directly or through
    a helper they call;
  * the regclass manual ops reach the ownership check, memory_status reaches
    the superuser/pg_monitor check;
  * the status walk keeps the header check, the exact field-count check and
    the row-visibility filter;
  * the SQL block REVOKEs from PUBLIC exactly the functions the coordination
    rule names, grants memory_status to pg_monitor only, leaves index_status
    PUBLIC, comments every function, and its RETURNS TABLE columns match the
    daemon header (names) and field kinds (types);
  * every AS 'MODULE_PATHNAME', 'sym' has a PG_FUNCTION_INFO_V1(sym).

    python3 test/unit/check_i3b_sql_perms.py [ROOT]
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


def strip_c_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def c_functions(src):
    """name -> body for top-level function definitions (brace matched)."""
    out = {}
    # PG style puts the name at column 0 (return type on the line above);
    # one-line "static void f(...)" also matches. Indented code never does.
    for m in re.finditer(r"^(?:[\w\*]+[ \t]+\**)*(\w+)\s*\(([^;{}]*)\)\s*\{",
                         src, flags=re.M):
        name = m.group(1)
        if name in ("if", "for", "while", "switch"):
            continue
        i = m.end()
        depth = 1
        while depth and i < len(src):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        out[name] = src[m.end():i - 1]
    return out


def reaches(funcs, start, pred, seen=None):
    """True when start's body, or a function it calls (transitively), matches pred."""
    seen = seen or set()
    if start in seen or start not in funcs:
        return False
    seen.add(start)
    body = funcs[start]
    if pred(body):
        return True
    for callee in set(re.findall(r"\b(\w+)\s*\(", body)):
        if callee in funcs and reaches(funcs, callee, pred, seen):
            return True
    return False


SUPERUSER_GUARD = re.compile(r"if\s*\(\s*!\s*superuser\s*\(\s*\)\s*\)")

c_src = strip_c_comments(read("src/pg_cuvs.c"))
funcs = c_functions(c_src)

# ---- C privilege checks -------------------------------------------------
for fn in ("pg_cuvs_reclaim_set", "pg_cuvs_unload_oid", "pg_cuvs_unpin_oid"):
    check(fn in funcs, f"C function {fn} not found")
    check(reaches(funcs, fn, lambda b: bool(SUPERUSER_GUARD.search(b))),
          f"{fn} does not reach an `if (!superuser())` guard")

for fn in ("pg_cuvs_unload", "pg_cuvs_load", "pg_cuvs_pin", "pg_cuvs_unpin"):
    check(fn in funcs, f"C function {fn} not found")
    check(reaches(funcs, fn, lambda b: "cuvs_rel_ownercheck(" in b and "aclcheck_error" in b),
          f"{fn} does not reach the index ownership check")
    check(reaches(funcs, fn, lambda b: "RELKIND_INDEX" in b),
          f"{fn} does not reach the relkind check")

check("pg_cuvs_memory_status" in funcs and
      re.search(r"if\s*\(\s*!\s*cuvs_status_is_priv\s*\(\s*\)\s*\)",
                funcs.get("pg_cuvs_memory_status", "")) is not None,
      "pg_cuvs_memory_status does not reject non-privileged callers")
priv = funcs.get("cuvs_status_is_priv", "")
check("superuser()" in priv and "ROLE_PG_MONITOR" in priv and "has_privs_of_role" in priv,
      "cuvs_status_is_priv is not superuser || has_privs_of_role(pg_monitor)")

# ---- status walk --------------------------------------------------------
emit = funcs.get("cuvs_status_emit", "")
check(re.search(r"if\s*\(\s*!\s*cuvs_status_check_header\s*\([^;]*\)\s*\)\s*ereport\s*\(\s*ERROR",
                emit, flags=re.S) is not None,
      "cuvs_status_emit does not ERROR when the header check fails")
check(re.search(r"if\s*\(\s*cuvs_status_split\s*\([^;]*\)\s*!=\s*ncols\s*\)\s*ereport\s*\(\s*ERROR",
                emit, flags=re.S) is not None,
      "cuvs_status_emit does not ERROR on a wrong field count")
check(re.search(r"if\s*\(\s*!\s*cuvs_status_row_visible\s*\([^;]*\)\s*\)\s*continue\s*;",
                emit, flags=re.S) is not None,
      "cuvs_status_emit does not skip rows cuvs_status_row_visible rejects")
check(re.search(r"if\s*\(\s*r\s*<\s*0\s*\)\s*ereport\s*\(\s*ERROR", emit) is not None,
      "cuvs_status_emit does not ERROR on a truncated (unterminated) row")

# ---- SQL block ----------------------------------------------------------
sql_path = "sql/pg_cuvs--0.8.2--0.9.0.sql"
sql = read(sql_path)
m = re.search(r"^-- I3-B begin\n(.*?)^-- I3-B end\n?", sql, flags=re.S | re.M)
check(m is not None, "I3-B begin/end block missing")
block = m.group(1) if m else ""
outside = (sql[:m.start()] + sql[m.end():]) if m else sql
# I3-B only appends: its block comes after I3-A's, and outside it the upgrade
# script holds only comments, blank lines, the \echo guard and other set PRs'
# own begin/end blocks (no statement of I3-B's, nothing loose).
check(m is not None and sql.find("-- I3-A end") >= 0 and sql.find("-- I3-A end") < m.start(),
      "I3-B block is not appended after the I3-A block")
_in_other = None
for line in outside.splitlines():
    om = re.match(r"^-- (I3-A|I3-C|I3-D|I4|I3-E) (begin|end)\s*$", line)
    if om:
        _in_other = om.group(1) if om.group(2) == "begin" else None
        continue
    if _in_other:
        continue
    check(line.strip() == "" or line.startswith("--") or line.startswith("\\echo "),
          f"statement outside the I3-B block: {line!r}")


# #203 owns a separate additive SQL block, not an I3-B privilege surface. It
# lives in the two 0.9.1 scripts (the version in development when it landed);
# the two 0.9.0 scripts are frozen and must not carry it. The block is safe
# only with this exact, invoker-only delegation contract: no extra SQL, grants,
# or SECURITY DEFINER can hide here.
CONN_UPGRADE, CONN_INSTALL = "sql/pg_cuvs--0.9.0--0.9.1.sql", "sql/pg_cuvs--0.9.1.sql"
for rel in (sql_path, "sql/pg_cuvs--0.9.0.sql"):
    frozen = read(rel)
    check("CONN203" not in frozen and "pg_cuvs_hnsw_connectivity_status" not in frozen,
          f"{rel}: frozen 0.9.0 script must not carry the #203 status function")
conn_pattern = r"^-- CONN203 begin\n(.*?)^-- CONN203 end\n?"
conn_blocks = []
conn_function = """CREATE FUNCTION pg_cuvs_hnsw_connectivity_status(index regclass) RETURNS jsonb
LANGUAGE SQL STABLE STRICT AS $$
 SELECT COALESCE(pg_cuvs_hnsw_build_info($1)->'connectivity',
                 '{"status":"not_recorded","scope":"build_snapshot"}'::jsonb)
$$;"""
normalize_sql = lambda text: re.sub(r"\s+", " ", text).strip()
for rel in (CONN_UPGRADE, CONN_INSTALL):
    text = read(rel)
    matches = list(re.finditer(conn_pattern, text, flags=re.S | re.M))
    check(len(matches) == 1 and text.count("-- CONN203 begin") == 1 and
          text.count("-- CONN203 end") == 1,
          f"{rel}: exactly one CONN203 begin/end block required")
    if matches:
        check(all(not line.strip() or line.startswith("--")
                  for line in text[matches[0].end():].splitlines()),
              f"{rel}: loose SQL after CONN203 block")
        before = text[:matches[0].start()]
        if rel == CONN_INSTALL:
            check(0 <= before.find("-- I3-E end"), f"{rel}: CONN203 block must follow I3-E")
        else:
            # The upgrade script holds nothing but the 0.9.1 additions: no SQL ahead of the block.
            check(all(not line.strip() or line.startswith("--") or line.startswith("\\echo")
                      for line in before.splitlines()),
                  f"{rel}: loose SQL before CONN203 block")
    body = matches[0].group(1) if matches else ""
    conn_blocks.append(body)
    code = "\n".join(line for line in body.splitlines()
                     if not line.lstrip().startswith("--"))
    expected = (re.escape(normalize_sql(conn_function)) +
                r" COMMENT ON FUNCTION pg_cuvs_hnsw_connectivity_status\(regclass\) IS "
                r"'(?:[^']|'')*';")
    check(re.fullmatch(expected, normalize_sql(code)) is not None,
          f"{rel}: CONN203 must contain only its exact status delegation and COMMENT")
check(conn_blocks[0] == conn_blocks[1],
      "CONN203 block differs between install and upgrade")

# The COMMENT is what \df+ shows. It must name the setting that decides whether a
# snapshot exists and must not promise a maintenance_work_mem-only check (larger
# checks continue in file-backed scratch). The development installer must carry
# the very same function and COMMENT as the fresh-install block.
comment_re = (r"COMMENT ON FUNCTION pg_cuvs_hnsw_connectivity_status\(regclass\) IS\s*"
              r"'((?:[^']|'')*)';")
conn_comments = []
for body in conn_blocks:
    m = re.search(comment_re, body)
    conn_comments.append(m.group(1) if m else "")
check(all(c for c in conn_comments), "CONN203 COMMENT not found")
for c in conn_comments:
    check("cuvs.hnsw_connectivity_check" in c and "disabled" in c and "not_recorded" in c,
          "CONN203 COMMENT must name cuvs.hnsw_connectivity_check and the disabled/not_recorded states")
    check("within maintenance_work_mem" not in c,
          "CONN203 COMMENT must not say the check stays within maintenance_work_mem")
installer = "scripts/install-connectivity-status.sql"
if os.path.exists(os.path.join(ROOT, installer)):
    inst = read(installer)
    code = "\n".join(line for line in inst.splitlines() if not line.lstrip().startswith("--"))
    im = re.search(comment_re, code)
    check(im is not None and im.group(1) == conn_comments[1],
          f"{installer}: COMMENT missing or differs from the CONN203 block")
    fn = normalize_sql(conn_function.replace("CREATE FUNCTION", "CREATE OR REPLACE FUNCTION"))
    check(fn in normalize_sql(code), f"{installer}: status function differs from the CONN203 block")
else:
    check(False, f"{installer} missing")


def norm_sig(name, args):
    types = []
    for a in [x.strip() for x in args.split(",") if x.strip()]:
        parts = a.split()
        types.append(parts[-1].lower())
    return f"{name}({','.join(types)})"


creates = {}
for cm in re.finditer(r"CREATE FUNCTION\s+(\w+)\s*\(([^)]*)\)(.*?);", block, flags=re.S):
    sig = norm_sig(cm.group(1), cm.group(2))
    creates[sig] = cm.group(3)

EXPECTED = {
    "pg_cuvs_unload(regclass)", "pg_cuvs_load(regclass)", "pg_cuvs_pin(regclass)",
    "pg_cuvs_unpin(regclass)", "pg_cuvs_unload(oid,oid)", "pg_cuvs_unpin(oid,oid)",
    "pg_cuvs_reclaim_set(text,text)", "pg_cuvs_index_status()", "pg_cuvs_memory_status()",
    "pg_cuvs_circuit_status()",
}
check(set(creates) == EXPECTED, f"CREATE FUNCTION set differs: {sorted(set(creates) ^ EXPECTED)}")

revoked = {norm_sig(n, a) for n, a in re.findall(
    r"REVOKE ALL ON FUNCTION\s+(\w+)\s*\(([^)]*)\)\s+FROM PUBLIC\s*;", block)}
WANT_REVOKED = EXPECTED - {"pg_cuvs_index_status()", "pg_cuvs_circuit_status()",
                           "pg_cuvs_unload(regclass)", "pg_cuvs_load(regclass)",
                           "pg_cuvs_pin(regclass)", "pg_cuvs_unpin(regclass)"}
check(revoked == WANT_REVOKED,
      f"REVOKE FROM PUBLIC set differs: missing={sorted(WANT_REVOKED - revoked)} "
      f"extra={sorted(revoked - WANT_REVOKED)}")
grants = re.findall(r"GRANT\s+(\w+)\s+ON FUNCTION\s+(\w+)\s*\(([^)]*)\)\s+TO\s+(\w+)\s*;", block)
check([(g[0], norm_sig(g[1], g[2]), g[3]) for g in grants] ==
      [("EXECUTE", "pg_cuvs_memory_status()", "pg_monitor")],
      f"GRANTs differ from memory_status -> pg_monitor only: {grants}")

commented = {norm_sig(n, a) for n, a in re.findall(
    r"COMMENT ON FUNCTION\s+(\w+)\s*\(([^)]*)\)\s+IS", block)}
check(commented == EXPECTED, f"COMMENT ON FUNCTION missing for {sorted(EXPECTED - commented)}")

c_symbols = set(re.findall(r"PG_FUNCTION_INFO_V1\((\w+)\)", c_src))
for sig, rest in creates.items():
    sm = re.search(r"AS\s+'MODULE_PATHNAME'\s*,\s*'(\w+)'", rest)
    check(sm is not None, f"{sig}: not AS 'MODULE_PATHNAME', '<symbol>'")
    if sm:
        check(sm.group(1) in c_symbols, f"{sig}: C symbol {sm.group(1)} has no PG_FUNCTION_INFO_V1")
    check(re.search(r"LANGUAGE C\b", rest) is not None, f"{sig}: not LANGUAGE C")
    if sig not in ("pg_cuvs_index_status()", "pg_cuvs_memory_status()"):
        check(re.search(r"\bSTRICT\b", rest) is not None, f"{sig}: not STRICT")
    check(re.search(r"\bVOLATILE\b", rest) is not None, f"{sig}: not VOLATILE")

# ---- RETURNS TABLE vs daemon header + kinds -----------------------------
ipc = read("src/cuvs_ipc.h")
kinds_h = read("src/cuvs_status_text.h")


def macro_string(text, name):
    mm = re.search(r"#define\s+" + name + r"\s+((?:\\\n|[^\n])*)", text)
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', mm.group(1))) if mm else None


SQL_TYPE = {"o": "oid", "i": "integer", "g": "integer", "l": "bigint",
            "t": "timestamptz", "b": "boolean", "r": "boolean", "s": "text"}


def returns_table(fn):
    rm = re.search(r"CREATE FUNCTION\s+" + fn + r"\s*\(\s*\)\s*RETURNS TABLE\s*\((.*?)\)\s*AS",
                   block, flags=re.S)
    if not rm:
        return None
    return [tuple(c.split()) for c in [x.strip() for x in rm.group(1).split(",")] if c]


for fn, cols_macro, kinds_macro, lead in (
        ("pg_cuvs_index_status", "CUVS_INDEX_STATUS_COLS", "CUVS_INDEX_STATUS_KINDS",
         [("index_name", "text")]),
        ("pg_cuvs_memory_status", "CUVS_MEMORY_STATUS_COLS", "CUVS_MEMORY_STATUS_KINDS", [])):
    names = macro_string(ipc, cols_macro).replace("\\t", "\t").split("\t")
    kinds = macro_string(kinds_h, kinds_macro)
    want = lead + [(n, SQL_TYPE[k]) for n, k in zip(names, kinds)]
    got = returns_table(fn)
    check(len(names) == len(kinds), f"{kinds_macro} length {len(kinds)} != {len(names)} columns")
    check(got == want, f"{fn} RETURNS TABLE differs from header/kinds:\n  got  {got}\n  want {want}")

# Coordination rule: the I3-B block is identical in the upgrade script that
# introduced it and in every install script from 0.9.0 on: pg_cuvs--0.9.0.sql
# (frozen) and the newest one (upgrade_path compares the two catalogs).
def _block(rel):
    t = read(rel)
    a, b = t.find("-- I3-B begin"), t.find("-- I3-B end")
    return t[a:b] if a >= 0 and b > a else None


def install_scripts():
    """pg_cuvs--0.9.0.sql and the newest full install script in sql/."""
    versions = []
    for n in os.listdir(os.path.join(ROOT, "sql")):
        vm = re.fullmatch(r"pg_cuvs--(\d+)\.(\d+)\.(\d+)\.sql", n)
        if vm:
            versions.append((tuple(int(x) for x in vm.groups()), "sql/" + n))
    newest = max(versions)[1] if versions else "sql/pg_cuvs--0.9.0.sql"
    return ["sql/pg_cuvs--0.9.0.sql"] + ([newest] if newest != "sql/pg_cuvs--0.9.0.sql" else [])


for _rel in install_scripts():
    _inst = _block(_rel) if os.path.exists(os.path.join(ROOT, _rel)) else None
    check(_inst is not None and _inst == _block("sql/pg_cuvs--0.8.2--0.9.0.sql"),
          f"I3-B block differs between {os.path.basename(_rel)} and pg_cuvs--0.8.2--0.9.0.sql")

if FAILS:
    for f in FAILS:
        print("FAIL", f)
    print(f"check_i3b_sql_perms: {len(FAILS)} failure(s)")
    sys.exit(1)
print("check_i3b_sql_perms: OK")
