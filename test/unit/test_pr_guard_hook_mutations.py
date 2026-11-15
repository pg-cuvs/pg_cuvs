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

FIND_PR = "if not any(AFTER_GH.match(cmd, w.end(), b) for w, b in zip(words, bounds)):"
# The pattern before the review: `gh` only at the start or after ; & | ( or a space.
OLD_FIND_PR = r'if not re.search(r"(^|[;&|(\s])gh\s+pr\s+(create|ready)\b", cmd):'
UNRESOLVED = '    print(cwd)\n    print("@self")'

# name: (text in the hook, planted replacement)
MUTANTS = {
    "old_regex": (FIND_PR, OLD_FIND_PR),
    # Every `gh` scans to the end of the command: quadratic on `gh -a gh -a ...`.
    "scan_not_bounded_by_next_gh": ("bounds = [w.start() for w in words[1:]] + [len(cmd)]",
                                    "bounds = [len(cmd)] * len(words)"),
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
