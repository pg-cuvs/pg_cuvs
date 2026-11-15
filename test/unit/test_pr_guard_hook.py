"""Tests of .claude/hooks/pr-guard.sh, the PreToolUse hook that runs
scripts/check-sdk-patch-path.sh before `gh pr create` / `gh pr ready`.

Each test feeds the hook a Claude Code tool-call payload on stdin and reads its
exit code (2 = blocked) and stderr. The checkouts are throwaway git repositories
that hold `scripts/prepare-patched-cuvs-source.sh` (what marks a pg_cuvs
checkout) and a stub `scripts/check-sdk-patch-path.sh` that exits 0 or 1 and
records the --root it was given. Python 3 standard library only; runs on macOS
and Linux. PR_GUARD_HOOK overrides the hook under test (the mutation test uses it).
"""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASH = shutil.which("bash")
HOOK = os.environ.get("PR_GUARD_HOOK") or str(ROOT / ".claude" / "hooks" / "pr-guard.sh")


def make_checkout(path, passes):
    """A git repository the hook takes for a pg_cuvs checkout."""
    scripts = path / "scripts"
    scripts.mkdir(parents=True)
    (scripts / "prepare-patched-cuvs-source.sh").write_text("#!/bin/sh\n")
    set_check(path, passes)
    subprocess.run(["git", "init", "-q", str(path)], check=True)
    return path


def set_check(path, passes, stream="stdout"):
    """Stub of scripts/check-sdk-patch-path.sh. Like the real script it accepts
    only `--root DIR` (anything else: usage, exit 2) and records DIR. A failing
    stub reports on stdout, or on stderr with stream="stderr"."""
    check = path / "scripts" / "check-sdk-patch-path.sh"
    redirect = " >&2" if stream == "stderr" else ""
    check.write_text(
        "#!/bin/sh\n"
        '[ "$1" = "--root" ] && [ -n "$2" ] && [ "$#" = 2 ] || '
        '{ echo "usage: $0 [--root DIR]" >&2; exit 2; }\n'
        'echo "$2" >> "$(dirname "$0")/calls"\n'
        + ('echo "stub: ok"\nexit 0\n' if passes else
           f'echo "stub: patched path broken ({stream})"{redirect}\nexit 1\n')
    )
    check.chmod(0o755)


def calls(path):
    f = path / "scripts" / "calls"
    if not f.exists():
        return []
    return [os.path.realpath(x) for x in f.read_text().split()]


class HookCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(os.path.realpath(self._tmp.name))
        # The session's own checkout: CLAUDE_PROJECT_DIR, where the bypass file lives.
        self.proj = make_checkout(self.tmp / "proj", passes=True)

    def run_hook(self, command, cwd=None, raw=None, env_extra=None, path_env=None, timeout=60):
        payload = raw if raw is not None else json.dumps(
            {"tool_input": {"command": command}, "cwd": str(cwd or self.proj)})
        env = dict(os.environ, CLAUDE_PROJECT_DIR=str(self.proj))
        env.pop("PGCUVS_CUVS_GIT", None)
        if path_env is not None:
            env["PATH"] = path_env
        env.update(env_extra or {})
        try:
            p = subprocess.run([BASH, HOOK], input=payload, capture_output=True,
                               text=True, env=env, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"hook still running after {timeout} s on a {len(payload)} byte payload")
        return p.returncode, p.stderr

    def assert_blocked(self, command, **kw):
        rc, err = self.run_hook(command, **kw)
        self.assertEqual(rc, 2, f"{command!r} was not blocked: rc={rc} stderr={err}")
        self.assertIn("BLOCKED", err)

    def assert_passes(self, command, **kw):
        rc, err = self.run_hook(command, **kw)
        self.assertEqual(rc, 0, f"{command!r} was blocked: rc={rc} stderr={err}")

    def bypass(self, text):
        d = self.proj / ".claude" / "state"
        d.mkdir(parents=True, exist_ok=True)
        (d / "pr-guard-bypass").write_text(text)


SPELLINGS = [
    "gh pr create --title t --body b",
    "gh pr ready 12",
    "git push -u origin HEAD && gh pr create --fill",
    "echo done; gh pr ready",
    "(gh pr create)",
    "true | gh pr create",
    "bash -c 'gh pr create'",
    'sh -c "gh pr create"',
    'eval "gh pr create"',
    "gh -R foo/bar pr create",
    "gh --repo foo/bar pr create --draft",
    "gh --repo=foo/bar pr ready",
    "/opt/homebrew/bin/gh pr create",
    "cd . && gh pr create",
]

NOT_A_PR = [
    "gh pr view 12",
    "gh pr list",
    "gh pr checks",
    "gh issue create --title t",
    "gh issue comment 3 --body ok",
    "git commit -m 'work on the pr'",
    "github pr create",
    "git-gh pr create",
    "make test-unit",
    "",
]

# cd targets the hook cannot resolve to an existing directory.
UNRESOLVED_CD = [
    "cd $M && gh pr create",
    'cd "$M" && gh pr create',
    "cd $(pwd)/x && gh pr create",
    "cd `pwd`/x && gh pr create",
    "cd ~/pg-cuvs-hook-test-no-such-dir && gh pr create",
    "cd /pg-cuvs-hook-test/no/such/dir; gh pr ready",
]


class SpellingTests(HookCase):
    def test_every_spelling_blocks_when_the_check_fails(self):
        set_check(self.proj, passes=False)
        for cmd in SPELLINGS:
            with self.subTest(cmd=cmd):
                self.assert_blocked(cmd)

    def test_every_spelling_passes_when_the_check_passes(self):
        for cmd in SPELLINGS:
            with self.subTest(cmd=cmd):
                self.assert_passes(cmd)
        self.assertTrue(calls(self.proj), "the stub check was never run")

    def test_other_commands_pass_even_when_the_check_fails(self):
        set_check(self.proj, passes=False)
        for cmd in NOT_A_PR:
            with self.subTest(cmd=cmd):
                self.assert_passes(cmd)
        self.assertEqual(calls(self.proj), [], "the check ran for a command that opens no PR")

    def test_block_message_names_the_failing_checkout_and_the_bypass(self):
        set_check(self.proj, passes=False)
        rc, err = self.run_hook("gh pr create")
        self.assertEqual(rc, 2)
        self.assertIn(str(self.proj), err)
        self.assertIn("stub: patched path broken (stdout)", err)
        self.assertIn("pr-guard-bypass", err)

    def test_check_is_called_as_the_real_script_expects(self):
        # The stub exits 2 with a usage line on anything but `--root DIR`, so a
        # hook that passes another flag sees a failing check and blocks here.
        self.assert_passes("gh pr create")
        self.assertEqual(calls(self.proj), [str(self.proj)])

    def test_stderr_of_the_check_is_in_the_block_message(self):
        set_check(self.proj, passes=False, stream="stderr")
        rc, err = self.run_hook("gh pr create")
        self.assertEqual(rc, 2)
        self.assertIn("stub: patched path broken (stderr)", err)


class FailOpenTests(HookCase):
    def setUp(self):
        super().setUp()
        set_check(self.proj, passes=False)

    def test_malformed_json_passes(self):
        for raw in ["{not json", "[1, 2", '"gh pr create"']:
            with self.subTest(raw=raw):
                rc, _ = self.run_hook(None, raw=raw)
                self.assertEqual(rc, 0)

    def test_empty_input_passes(self):
        rc, _ = self.run_hook(None, raw="")
        self.assertEqual(rc, 0)

    def test_payload_without_a_command_passes(self):
        rc, _ = self.run_hook(None, raw=json.dumps({"cwd": str(self.proj)}))
        self.assertEqual(rc, 0)

    def test_checkout_that_is_not_pg_cuvs_passes(self):
        other = self.tmp / "other"
        other.mkdir()
        subprocess.run(["git", "init", "-q", str(other)], check=True)
        self.assert_passes("gh pr create", cwd=other)

    def test_directory_that_is_not_a_git_checkout_passes(self):
        plain = self.tmp / "plain"
        plain.mkdir()
        self.assert_passes("gh pr create", cwd=plain)

    def test_missing_python3_passes(self):
        # PATH holds `cat` only: the hook cannot parse the payload and must not block.
        bindir = self.tmp / "bin"
        bindir.mkdir()
        (bindir / "cat").symlink_to(shutil.which("cat"))
        rc, _ = self.run_hook("gh pr create", path_env=str(bindir))
        self.assertEqual(rc, 0)


class BypassTests(HookCase):
    def setUp(self):
        super().setUp()
        set_check(self.proj, passes=False)

    def test_unexpired_bypass_passes(self):
        self.bypass(f"until={int(time.time()) + 600}\nreason=network down\n")
        rc, err = self.run_hook("gh pr create")
        self.assertEqual(rc, 0)
        self.assertIn("network down", err)

    def test_expired_bypass_blocks(self):
        self.bypass(f"until={int(time.time()) - 5}\nreason=old\n")
        self.assert_blocked("gh pr create")

    def test_bypass_of_the_session_checkout_covers_another_checkout(self):
        wt = make_checkout(self.tmp / "wt", passes=False)
        self.bypass(f"until={int(time.time()) + 600}\nreason=session wide\n")
        self.assert_passes("gh pr create", cwd=wt)

    def test_bypass_file_in_the_checked_checkout_is_ignored(self):
        # Only the session checkout (CLAUDE_PROJECT_DIR) may hold the bypass.
        wt = make_checkout(self.tmp / "wt", passes=False)
        state = wt / ".claude" / "state"
        state.mkdir(parents=True)
        (state / "pr-guard-bypass").write_text(f"until={int(time.time()) + 600}\nreason=planted\n")
        self.assert_blocked("gh pr create", cwd=wt)

    def test_garbage_bypass_blocks(self):
        for text in ["until=soon\nreason=x\n", "reason=no until line\n", "", "until=\n",
                     "until=-1\n", "garbage\n"]:
            with self.subTest(text=text):
                self.bypass(text)
                self.assert_blocked("gh pr create")


class CwdAndCdTests(HookCase):
    def test_checks_the_checkout_of_the_tool_call_cwd(self):
        wt = make_checkout(self.tmp / "wt", passes=False)
        self.assert_blocked("gh pr create", cwd=wt)
        self.assertEqual(calls(wt), [str(wt)])
        self.assertEqual(calls(self.proj), [], "the session checkout was checked, not the cwd")

    def test_root_passed_to_the_check_is_the_top_level_of_a_subdirectory_cwd(self):
        sub = self.proj / "src" / "deep"
        sub.mkdir(parents=True)
        self.assert_passes("gh pr create", cwd=sub)
        self.assertEqual(calls(self.proj), [str(self.proj)])

    def test_checkout_without_the_script_is_checked_with_the_session_copy(self):
        wt = make_checkout(self.tmp / "wt", passes=True)
        (wt / "scripts" / "check-sdk-patch-path.sh").unlink()
        set_check(self.proj, passes=False)
        self.assert_blocked("gh pr create", cwd=wt)
        self.assertEqual(calls(self.proj), [str(wt)], "the session copy runs with --root of the checked tree")
        set_check(self.proj, passes=True)
        self.assert_passes("gh pr create", cwd=wt)

    def test_no_script_anywhere_passes_with_a_notice(self):
        wt = make_checkout(self.tmp / "wt", passes=True)
        (wt / "scripts" / "check-sdk-patch-path.sh").unlink()
        (self.proj / "scripts" / "check-sdk-patch-path.sh").unlink()
        rc, err = self.run_hook("gh pr create", cwd=wt)
        self.assertEqual(rc, 0)
        self.assertIn("check script not found", err)

    def test_a_resolvable_cd_selects_the_checkout_to_check(self):
        wt = make_checkout(self.tmp / "wt", passes=True)
        set_check(self.proj, passes=False)
        self.assert_passes(f"cd {wt} && gh pr create")
        self.assertEqual(calls(wt), [str(wt)])
        self.assertEqual(calls(self.proj), [])
        set_check(wt, passes=False)
        set_check(self.proj, passes=True)
        self.assert_blocked(f"cd {wt}; gh pr ready")
        self.assert_blocked(f'cd "{wt}" && gh pr create')
        self.assert_blocked("cd ../wt && gh pr create", cwd=self.proj)

    def test_unresolved_cd_checks_the_cwd_and_the_session_checkout(self):
        cwd = make_checkout(self.tmp / "cwd", passes=True)
        for cmd in UNRESOLVED_CD:
            for cwd_ok, proj_ok, expect in [(True, True, 0), (False, True, 2),
                                            (True, False, 2), (False, False, 2)]:
                with self.subTest(cmd=cmd, cwd_ok=cwd_ok, proj_ok=proj_ok):
                    set_check(cwd, cwd_ok)
                    set_check(self.proj, proj_ok)
                    rc, err = self.run_hook(cmd, cwd=cwd, env_extra={"M": "/nonexistent-m"})
                    self.assertEqual(rc, expect, err)

    def test_unresolved_cd_checks_both_trees_not_just_the_first_failure(self):
        cwd = make_checkout(self.tmp / "cwd", passes=False)
        set_check(self.proj, passes=False)
        rc, err = self.run_hook("cd $M && gh pr create", cwd=cwd)
        self.assertEqual(rc, 2)
        self.assertIn(f"failed for {cwd}", err)
        self.assertIn(f"failed for {self.proj}", err)

    def test_unresolved_cd_in_the_session_checkout_is_checked_once(self):
        set_check(self.proj, passes=True)
        self.assert_passes("cd $M && gh pr create", cwd=self.proj)
        self.assertEqual(calls(self.proj), [str(self.proj)])

    def test_unresolved_cd_outside_any_checkout_still_checks_the_session_checkout(self):
        plain = self.tmp / "plain"
        plain.mkdir()
        set_check(self.proj, passes=False)
        self.assert_blocked("cd $M && gh pr create", cwd=plain)


class ScalingTests(HookCase):
    """The command is user input of any size (a heredoc, a pasted log): finding
    `gh pr create` in it must take time linear in its length."""

    def test_repeated_gh_options_take_linear_time(self):
        set_check(self.proj, passes=False)
        for unit in ["gh -a ", "gh -x\n"]:
            t = {}
            for n in (4000, 16000):  # about 24 KB and 96 KB
                start = time.monotonic()
                self.assert_passes(unit * n, timeout=10)
                t[n] = time.monotonic() - start
                self.assertLess(t[n], 2.0, f"{unit!r} x {n}")
            # Four times the input: a quadratic scan takes about sixteen times as long.
            self.assertLess(t[16000], 6 * max(t[4000], 0.3), f"{unit!r}: {t}")

    def test_a_pr_after_a_long_run_of_gh_words_is_still_found(self):
        set_check(self.proj, passes=False)
        self.assert_blocked("gh -a " * 16000 + "gh pr create", timeout=10)


if __name__ == "__main__":
    unittest.main(verbosity=1)
