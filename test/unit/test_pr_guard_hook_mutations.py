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

GH_PR_RE = r'GH_PR_RE = r"(?<![\w-])gh(?:\s+-\S+(?:\s+[^\s-][^\s;&|()]*)?)*\s+pr\s+(?:create|ready)\b"'
# The pattern before the review: `gh` only at the start or after ; & | ( or a space.
OLD_GH_PR_RE = r'GH_PR_RE = r"(^|[;&|(\s])gh\s+pr\s+(create|ready)\b"'
UNRESOLVED = '    print(cwd)\n    print("@self")'

# name: (text in the hook, planted replacement)
MUTANTS = {
    "old_regex": (GH_PR_RE, OLD_GH_PR_RE),
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
