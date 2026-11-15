#!/usr/bin/env bash
# PreToolUse(Bash) guard: run scripts/check-sdk-patch-path.sh before a pull request is opened.
#
# Rationale: 0.9.1 shipped a Makefile that could not compile src/cuvs_wrapper.cu with the
# patched cuVS SDK planner (missing --expt-relaxed-constexpr). Neither CI job builds that
# path, so nothing failed before the release. The check needs no CUDA and takes seconds
# once the pinned cuVS source is cached.
#
# Scope: `gh pr create` and `gh pr ready` in a pg_cuvs checkout. Every other command passes.
# The checkout checked is the one the command runs in (a leading `cd DIR &&`, else the
# tool call's cwd), so a PR opened from a worktree checks that worktree.
#
# Escape hatch, explicit and expiring (gitignored):
#   .claude/state/pr-guard-bypass    until=<epoch> reason=<text>
set -uo pipefail

# A hook that dies must not block the tool call, so every parse failure exits 0.
parsed=$(cat | python3 -c '
import json, os, re, shlex, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
cmd = ((d.get("tool_input") or {}).get("command", "") or "")
if not re.search(r"(^|[;&|(\s])gh\s+pr\s+(create|ready)\b", cmd):
    sys.exit(0)
where = d.get("cwd") or os.getcwd()
m = re.match(r"\s*cd\s+(\S+)\s*(&&|;)", cmd)
if m:
    try:
        target = shlex.split(m.group(1))[0]
    except ValueError:
        target = m.group(1)
    target = os.path.expanduser(target)
    where = target if os.path.isabs(target) else os.path.join(where, target)
print(where)
print(d.get("agent_type") or "main")
') || exit 0
where=$(printf '%s\n' "$parsed" | sed -n 1p)
agent=$(printf '%s\n' "$parsed" | sed -n 2p)
[ -n "$where" ] || exit 0

root=$(git -C "$where" rev-parse --show-toplevel 2>/dev/null) || exit 0
[ -f "$root/scripts/prepare-patched-cuvs-source.sh" ] || exit 0   # not a pg_cuvs checkout

self_root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
bypass="$self_root/.claude/state/pr-guard-bypass"
if [ -f "$bypass" ]; then
    until_=$(sed -n 's/^until=\([0-9]*\).*/\1/p' "$bypass" | head -1)
    if [ -n "$until_" ] && [ "$(date +%s)" -lt "$until_" ]; then
        echo "[pr-guard] bypass active: $(sed -n 's/^reason=//p' "$bypass" | head -1)" >&2
        exit 0
    fi
fi

# Prefer the checked checkout's own script; a branch made before the script existed is
# checked with this repository's copy.
check="$root/scripts/check-sdk-patch-path.sh"
[ -x "$check" ] || check="$self_root/scripts/check-sdk-patch-path.sh"
[ -x "$check" ] || { echo "[pr-guard] check script not found; not blocking" >&2; exit 0; }

if out=$("$check" --root "$root" 2>&1); then
    exit 0
fi
cat >&2 <<EOF
[pr-guard] BLOCKED: the patched cuVS SDK path check failed for $root (caller: $agent).

$(printf '%s\n' "$out" | tail -15)

Run it yourself: scripts/check-sdk-patch-path.sh --root $root
Fix the cause before opening the pull request. Genuine exception (for example, no network
for the first fetch of the pinned cuVS source): create a short-lived, attributable bypass —
  mkdir -p .claude/state
  printf 'until=%s\nreason=<why>\n' "\$(( \$(date +%s) + 1800 ))" > .claude/state/pr-guard-bypass
EOF
exit 2
