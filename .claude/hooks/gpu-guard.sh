#!/usr/bin/env bash
# PreToolUse(Bash) guard: long-running GPU VM jobs must go through gpu-run.sh.
#
# Rationale (see infra/scripts/gpu-run.sh header): launching a benchmark or
# installcheck by hand over ssh leaves no queryable record of whether it is
# running, no lock against a second actor starting the same job, and no
# preflight against stale artifacts. All three produced wrong conclusions.
#
# Blocks only *launches* of known long jobs over ssh. Reading logs, checking
# state, and every other ssh call pass through untouched.
#
# Escape hatch is itself a state file so an exception is explicit, attributable
# and expiring — not a habit:
#   .claude/state/gpu-bypass    (gitignored)   until=<epoch> reason=<text>
set -uo pipefail

# A hook that dies must not block the tool call, so every parse failure exits 0.
parsed=$(cat | python3 -c '
import json,sys
try:    d=json.load(sys.stdin)
except Exception: sys.exit(0)
cmd=((d.get("tool_input") or {}).get("command","") or "").replace("\n"," ")
print(cmd)
print(d.get("agent_type") or "main")
') || exit 0

cmd=$(printf '%s\n' "$parsed" | sed -n 1p)
agent=$(printf '%s\n' "$parsed" | sed -n 2p)
[ -n "$cmd" ] || exit 0

# Only ssh launches are in scope. gpu-run.sh runs locally, so it never matches.
case "$cmd" in *ssh*) ;; *) exit 0;; esac

# Known long-running jobs. Keep this list narrow — false blocks are worse than
# the occasional miss, because a blocked-by-mistake call trains people to bypass.
long_job=0
for pat in adr079_3o_recall.py run_pg_cuvsbench.py build_gt.py fetch_dataset.py \
           build_time_arm.py installcheck 'make gpu-'; do
    case "$cmd" in *"$pat"*) long_job=1; break;; esac
done
[ "$long_job" = "1" ] || exit 0

# Reading about a job is not launching one.
case "$cmd" in *"gpu-run.sh"*|*tail\ *|*"grep -c"*|*pgrep*) exit 0;; esac

# Bypass state file: present AND unexpired.
root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
bypass="$root/.claude/state/gpu-bypass"
if [ -f "$bypass" ]; then
    now=$(date +%s)
    until_=$(sed -n 's/^until=\([0-9]*\).*/\1/p' "$bypass" | head -1)
    if [ -n "$until_" ] && [ "$now" -lt "$until_" ]; then
        exit 0
    fi
    echo "[gpu-guard] bypass file present but expired ($(date -r "$bypass" '+%F %T' 2>/dev/null)); ignoring." >&2
fi

cat >&2 <<EOF
[gpu-guard] BLOCKED: launching a long GPU job over raw ssh (caller: $agent).

Use the runner instead — it takes an exclusive lock, records state, and refuses
to run against a stale daemon/extension:

  infra/scripts/gpu-run.sh run <name> -- <command>
  infra/scripts/gpu-run.sh status      # running? how did the last one end?
  infra/scripts/gpu-run.sh log 40

Why: an idle agent, a self-matching pgrep, and a launcher's exit code all look
like "finished" and are not. Two actors have already collided on one output file.

Genuine exception (debugging, one-off): create a short-lived, attributable bypass —
  mkdir -p .claude/state
  printf 'until=%s\nreason=<why>\n' "\$(( \$(date +%s) + 1800 ))" > .claude/state/gpu-bypass
EOF
exit 2
