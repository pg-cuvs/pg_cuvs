#!/usr/bin/env python3
"""Mutation checks for the scoped #203 SQL block and unchanged I3-B ACLs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UP = 'sql/pg_cuvs--0.8.2--0.9.0.sql'
INSTALL = 'sql/pg_cuvs--0.9.0.sql'


class SQLContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name in [UP, INSTALL, 'src/pg_cuvs.c', 'src/cuvs_ipc.h',
                     'src/cuvs_status_text.h']:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)

    def edit(self, name, old, new):
        p = self.root / name
        s = p.read_text()
        self.assertIn(old, s)
        p.write_text(s.replace(old, new, 1))

    def check(self, ok, message=''):
        r = subprocess.run([sys.executable, str(ROOT / 'test/unit/check_i3b_sql_perms.py'),
                            str(self.root)], capture_output=True, text=True)
        self.assertEqual(r.returncode == 0, ok, r.stdout + r.stderr)
        if message:
            self.assertIn(message, r.stdout)

    def test_current_tree(self):
        self.check(True)

    def test_original_loose_function(self):
        self.edit(UP, '-- CONN203 begin\n', '')
        self.edit(UP, '-- CONN203 end\n', '')
        self.check(False, 'statement outside the I3-B block')

    def test_i3b_revoke_still_required(self):
        self.edit(UP, 'REVOKE ALL ON FUNCTION pg_cuvs_reclaim_set(text, text) FROM PUBLIC;', '')
        self.check(False, 'REVOKE FROM PUBLIC set differs')

    def test_new_block_cannot_hide_grant(self):
        self.edit(UP, '-- CONN203 end', 'GRANT EXECUTE ON FUNCTION pg_cuvs_reclaim_set(text,text) TO PUBLIC;\n-- CONN203 end')
        self.check(False, 'CONN203 must contain only')

    def test_no_security_definer(self):
        self.edit(UP, 'LANGUAGE SQL STABLE STRICT AS $$', 'LANGUAGE SQL STABLE STRICT SECURITY DEFINER AS $$')
        self.check(False, 'CONN203 must contain only')

    def test_delegation_body_is_exact(self):
        self.edit(UP, "pg_cuvs_hnsw_build_info($1)->'connectivity'", "pg_cuvs_hnsw_build_info($1)->'wrong_key'")
        self.check(False, 'CONN203 must contain only')

    def test_comment_cannot_hide_statement(self):
        self.edit(UP, '-- CONN203 end', "COMMENT ON FUNCTION pg_cuvs_reclaim_set(text,text) IS 'hidden';\n-- CONN203 end")
        self.check(False, 'CONN203 must contain only')

    def test_duplicate_owner_block(self):
        self.edit(UP, '-- CONN203 end', '-- CONN203 end\n-- CONN203 begin\n-- CONN203 end')
        self.check(False, 'exactly one CONN203')

    def test_install_upgrade_must_match(self):
        self.edit(INSTALL, 'Last build structural connectivity snapshot;', 'Different description;')
        self.check(False, 'CONN203 block differs')

    def test_install_trailing_sql_is_rejected(self):
        self.edit(INSTALL, '-- CONN203 end', '-- CONN203 end\nSELECT 1;')
        self.check(False, 'loose SQL after CONN203')


if __name__ == '__main__':
    unittest.main()
