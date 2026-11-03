#!/usr/bin/env python3
"""Mutation check for the I3-B status/permission surface.

Each mutation injects one defect into a temp copy of src/ + sql/ + test/unit/
and requires the named check to FAIL (RED):
  unit -> test/unit/test_status_text.c against src/cuvs_status_text.c
  scan -> test/unit/check_i3b_sql_perms.py against the mutated tree
The unmutated tree must pass both (GREEN).

    python3 test/unit/mutate_status_text.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CC = os.environ.get("CC", "cc")

# (id, check, file, old, new)
MUTATIONS = [
    ("a1 visibility ignores ownership", "unit", "src/cuvs_status_text.c",
     "return is_priv || (row_db == my_db && owns);",
     "return is_priv || row_db == my_db;"),
    ("a2 visibility ignores database", "unit", "src/cuvs_status_text.c",
     "return is_priv || (row_db == my_db && owns);",
     "return is_priv || owns;"),
    ("a3 caller drops the visibility filter", "scan", "src/pg_cuvs.c",
     "            if (!cuvs_status_row_visible(is_priv, row_db, MyDatabaseId, owns))\n"
     "                continue;\n",
     ""),
    ("b1 header compare removed", "unit", "src/cuvs_status_text.c",
     "    if (line_len != strlen(header) || memcmp(line, header, line_len) != 0)\n"
     "        return 0;\n",
     ""),
    ("b2 caller ignores header mismatch", "scan", "src/pg_cuvs.c",
     "    if (!cuvs_status_check_header(text, len, header, &pos))\n        ereport(ERROR,",
     "    if (!cuvs_status_check_header(text, len, header, &pos))\n        ereport(WARNING,"),
    ("c1 split accepts too few fields", "unit", "src/cuvs_status_text.c",
     "    return count == n ? n : -1;",
     "    return n;"),
    ("c2 split accepts too many fields", "unit", "src/cuvs_status_text.c",
     "        if (count == n)\n            return -1;\n",
     "        if (count == n)\n            break;\n"),
    ("c3 caller ignores field count", "scan", "src/pg_cuvs.c",
     "if (cuvs_status_split((char *) line, line_len, f, ncols) != ncols)",
     "if (cuvs_status_split((char *) line, line_len, f, ncols) < 0 && false)"),
    ("c4 unterminated tail accepted", "unit", "src/cuvs_status_text.c",
     "    if (nl == NULL)\n        return -1;\n",
     "    if (nl == NULL)\n        return 0;\n"),
    ("d1 (oid, oid) overloads skip superuser", "scan", "src/pg_cuvs.c",
     "    cuvs_require_superuser(fn);\n", ""),
    ("d2 reclaim_set skips superuser", "scan", "src/pg_cuvs.c",
     "    cuvs_require_superuser(\"pg_cuvs_reclaim_set\");\n", ""),
    ("d3 superuser guard neutered", "scan", "src/pg_cuvs.c",
     "    if (!superuser())\n        ereport(ERROR,\n                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),\n"
     "                 errmsg(\"must be superuser to call %s\", fn)));",
     "    if (false)\n        ereport(ERROR,\n                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),\n"
     "                 errmsg(\"must be superuser to call %s\", fn)));"),
    ("d4 SQL keeps reclaim_set PUBLIC", "scan", "sql/pg_cuvs--0.8.2--0.9.0.sql",
     "REVOKE ALL ON FUNCTION pg_cuvs_reclaim_set(text, text) FROM PUBLIC;\n", ""),
    ("d5 SQL keeps unload(oid, oid) PUBLIC", "scan", "sql/pg_cuvs--0.8.2--0.9.0.sql",
     "REVOKE ALL ON FUNCTION pg_cuvs_unload(oid, oid)      FROM PUBLIC;\n", ""),
    ("d6 SQL keeps unpin(oid, oid) PUBLIC", "scan", "sql/pg_cuvs--0.8.2--0.9.0.sql",
     "REVOKE ALL ON FUNCTION pg_cuvs_unpin(oid, oid)       FROM PUBLIC;\n", ""),
    ("d7 SQL revokes unload(regclass) from PUBLIC (owner check is the gate)", "scan",
     "sql/pg_cuvs--0.8.2--0.9.0.sql",
     "REVOKE ALL ON FUNCTION pg_cuvs_unload(oid, oid)",
     "REVOKE ALL ON FUNCTION pg_cuvs_unload(regclass) FROM PUBLIC;\nREVOKE ALL ON FUNCTION pg_cuvs_unload(oid, oid)"),
    ("d8 SQL revokes index_status too", "scan", "sql/pg_cuvs--0.8.2--0.9.0.sql",
     "-- I3-B end", "REVOKE ALL ON FUNCTION pg_cuvs_index_status() FROM PUBLIC;\n-- I3-B end"),
    ("d9 SQL drops the pg_monitor grant", "scan", "sql/pg_cuvs--0.8.2--0.9.0.sql",
     "GRANT EXECUTE ON FUNCTION pg_cuvs_memory_status()    TO pg_monitor;\n", ""),
    ("d10 regclass ops skip ownership", "scan", "src/pg_cuvs.c",
     "    if (!cuvs_rel_ownercheck(relid, GetUserId()))\n"
     "        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, get_rel_name(relid));\n",
     ""),
    ("d11 memory_status skips the privilege check", "scan", "src/pg_cuvs.c",
     "    if (!cuvs_status_is_priv())\n        ereport(ERROR,",
     "    if (false)\n        ereport(ERROR,"),
]


def copy_tree(dst):
    for d in ("src", "sql", os.path.join("test", "unit")):
        shutil.copytree(os.path.join(ROOT, d), os.path.join(dst, d))


def run_unit(tree):
    exe = os.path.join(tree, "t")
    b = subprocess.run([CC, "-std=gnu11", "-Wall", "-Wextra", "-I", os.path.join(tree, "src"),
                        "-o", exe, os.path.join(tree, "test/unit/test_status_text.c"),
                        os.path.join(tree, "src/cuvs_status_text.c")],
                       capture_output=True, text=True)
    if b.returncode != 0:
        return False, "build failed: " + b.stderr.strip().splitlines()[-1]
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


def run_scan(tree):
    r = subprocess.run([sys.executable, os.path.join(tree, "test/unit/check_i3b_sql_perms.py"), tree],
                       capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


CHECKS = {"unit": run_unit, "scan": run_scan}


def first_fail(out):
    for line in out.splitlines():
        if line.startswith("FAIL"):
            return line
    return out.splitlines()[-1] if out else ""


def main():
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "base")
        copy_tree(base)
        for name, fn in CHECKS.items():
            ok, out = fn(base)
            print(f"GREEN {name}: {'PASS' if ok else 'FAIL'} | {out.splitlines()[-1]}")
            bad += not ok
        for mid, check, rel, old, new in MUTATIONS:
            tree = os.path.join(tmp, "m")
            shutil.rmtree(tree, ignore_errors=True)
            copy_tree(tree)
            path = os.path.join(tree, rel)
            with open(path) as f:
                src = f.read()
            if src.count(old) != 1:
                print(f"ERROR {mid}: anchor found {src.count(old)} times in {rel}")
                bad += 1
                continue
            with open(path, "w") as f:
                f.write(src.replace(old, new))
            ok, out = CHECKS[check](tree)
            print(f"{'SURVIVED' if ok else 'RED'} {mid} [{check}] | {first_fail(out)}")
            bad += ok
    print(f"mutate_status_text: {len(MUTATIONS)} mutations, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
