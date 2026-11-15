"""Mutations of .claude/hooks/pr-guard.sh: each must turn test_pr_guard_hook.py RED.

Every mutant is a copy of the hook with one planted defect; the unmutated hook
must pass the tests first. A mutation anchor that no longer matches the hook is
an error, so a rewrite of the hook cannot silently retire a mutant.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOOK = ROOT / ".claude" / "hooks" / "pr-guard.sh"
TESTS = ROOT / "test" / "unit" / "test_pr_guard_hook.py"

FIND_PR = "if not opens_pr(cmd):"
# The pattern before the review: `gh` only at the start or after ; & | ( or a space.
OLD_FIND_PR = r'if not re.search(r"(^|[;&|(\s])gh\s+pr\s+(create|ready)\b", cmd):'
UNRESOLVED = '    print(cwd)\n    print("@self")'

# One pattern for gh, options and `pr create`: right verdicts, quadratic time.
ONE_PATTERN = ('if not re.search(r"(?<![\\w-])gh(?:\\s+-\\S+(?:\\s+[^\\s-][^\\s;&|()]*)?)*'
               '\\s+pr\\s+(?:create|ready)\\b", cmd):')
# Each `gh` is scanned only up to the next `gh` word: a `gh` inside an option value ends
# the scan of the `gh` before it, so `gh -R gh/foo pr create` is not found.
BOUNDED_BY_NEXT_GH = (
    'words = list(re.finditer(r"(?<![\\w-])gh(?![\\w-])", cmd))\n'
    'bounds = [w.start() for w in words[1:]] + [len(cmd)]\n'
    'after = re.compile(r"(?:\\s+-\\S+(?:\\s+[^\\s-][^\\s;&|()]*)?)*\\s+pr\\s+(?:create|ready)\\b")\n'
    'if not any(after.match(cmd, w.end(), b) for w, b in zip(words, bounds)):')

# name: (text in the hook, planted replacement)
MUTANTS = {
    "old_regex": (FIND_PR, OLD_FIND_PR),
    "scan_bounded_by_next_gh": (FIND_PR, BOUNDED_BY_NEXT_GH),
    "one_pattern_quadratic": (FIND_PR, ONE_PATTERN),
    "only_first_gh_considered": (
        "return any(ok[i + 1] and GH_END.search(t) for i, t in enumerate(toks))",
        "return next((bool(ok[i + 1]) for i, t in enumerate(toks) if GH_END.search(t)), False)"),
    "option_needs_a_value": ("ok[i] = ok[i + 1] or (", "ok[i] = ("),
    "option_value_never_taken": (" or (i + 1 < n and ok[i + 2] and VALUE.match(toks[i + 1]) is not None)", ""),
    "option_value_not_skipped": ("ok[i + 2] and VALUE.match", "ok[i + 1] and VALUE.match"),
    "value_may_start_with_dash": ('VALUE = re.compile(r"[^\\s-]', 'VALUE = re.compile(r"[^\\s]'),
    "value_may_hold_separators": ('[^\\s;&|()]*\\Z")', '\\S*\\Z")'),
    "lone_dash_is_an_option": ('len(t) > 1', 'len(t) > 0'),
    "gh_after_word_char_or_dash": ('GH_END = re.compile(r"(?<![\\w-])gh\\Z")', 'GH_END = re.compile(r"gh\\Z")'),
    "gh_not_at_end_of_token": ('GH_END = re.compile(r"(?<![\\w-])gh\\Z")', 'GH_END = re.compile(r"(?<![\\w-])gh")'),
    "subcommand_without_word_boundary": ('SUBCOMMAND = re.compile(r"(?:create|ready)\\b")', 'SUBCOMMAND = re.compile(r"(?:create|ready)")'),
    "any_pr_subcommand": ("ok[i] = i + 1 < n and SUBCOMMAND.match(toks[i + 1]) is not None", "ok[i] = i + 1 < n"),
    "no_unresolved_cd_fallback": (UNRESOLVED, '    print("/nonexistent")'),
    "unresolved_cd_checks_cwd_only": (UNRESOLVED, "    print(cwd)"),
    "unresolved_cd_checks_project_only": (UNRESOLVED, '    print("@self")'),
    "always_exit_0": ("set -uo pipefail\n", "set -uo pipefail\nexit 0\n"),
    "always_exit_2": ("set -uo pipefail\n", "set -uo pipefail\nexit 2\n"),
    "expired_bypass_accepted": ('[ "$(date +%s)" -lt "$until_" ]', "true"),
    "bypass_ignored": ('if [ -f "$bypass" ]', "if false"),
    "stops_at_first_failing_tree": ("    failed=1\n", "    failed=1\n    break\n"),
    "same_tree_checked_twice": ("*$'\\n'\"$root\"$'\\n'*) continue;;", "*NEVER*) continue;;"),
    "non_pg_cuvs_checkout_checked": ('[ -f "$root/scripts/prepare-patched-cuvs-source.sh" ] || continue', "true"),
    "check_called_with_bogus_flag": ('"$check" --root "$root"', '"$check" --rooot "$root"'),
    "stderr_dropped_from_out": ('out=$("$check" --root "$root" 2>&1)', 'out=$("$check" --root "$root" 2>/dev/null)'),
    "bypass_at_checked_root": ('bypass="$self_root/.claude', 'bypass="${root:-$self_root}/.claude'),
    "no_self_script_fallback": ('    [ -x "$check" ] || check="$self_root/scripts/check-sdk-patch-path.sh"\n', ""),
    "root_not_toplevel": ('root=$(git -C "$where" rev-parse --show-toplevel 2>/dev/null) || continue', 'root="$where"'),
    "resolved_cd_ignored": ("if target:\n    print(target)", "if False:\n    print(target)"),
}


def run(hook_path, failfast):
    cmd = [sys.executable, "-I", "-B", str(TESTS)] + (["-f"] if failfast else [])
    p = subprocess.run(cmd, env=dict(os.environ, PR_GUARD_HOOK=str(hook_path)),
                       capture_output=True, text=True)
    return p.returncode, p.stderr


def main():
    text = HOOK.read_text()
    rc, err = run(HOOK, failfast=False)
    if rc != 0:
        sys.exit(f"BASELINE_NOT_GREEN: the unmutated hook fails its tests\n{err}")
    survivors = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, (old, new) in MUTANTS.items():
            if old not in text:
                sys.exit(f"mutation anchor not found in the hook ({name}): {old!r}")
            mutated = pathlib.Path(tmp) / f"{name}.sh"
            mutated.write_text(text.replace(old, new, 1))
            rc, _ = run(mutated, failfast=True)
            print(f"{'KILLED  ' if rc != 0 else 'SURVIVED'} {name}")
            if rc == 0:
                survivors.append(name)
    if survivors:
        sys.exit(f"surviving mutants: {', '.join(survivors)}")
    print(f"OK: {len(MUTANTS)} mutants, all killed")


if __name__ == "__main__":
    main()
