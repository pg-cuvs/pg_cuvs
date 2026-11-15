#!/usr/bin/env bash
# PreToolUse(Bash) guard: run scripts/check-sdk-patch-path.sh before a pull request is opened.
#
# Rationale: 0.9.1 shipped a Makefile that could not compile src/cuvs_wrapper.cu with the
# patched cuVS SDK planner (missing --expt-relaxed-constexpr). Neither CI job builds that
# path, so nothing failed before the release. The check needs no CUDA and takes seconds
# once the pinned cuVS source is cached.
#
# What it is: a guard against mistakes, not a security boundary. The bypass file below is
# made by a command the block message prints. It only runs in sessions that load this
# project's .claude/settings.json (started in a pg_cuvs checkout), and it checks the working
# tree, not the head of the pull request.
#
# Scope: `gh pr create` and `gh pr ready` in a pg_cuvs checkout; every other command passes.
# The spellings it finds: `gh` as a word anywhere in the command (so also inside
# `bash -c '...'`, `sh -c "..."`, `eval "..."`, after a path such as /opt/homebrew/bin/gh,
# and after other `&&`/`;`/`|` commands), then options such as `-R OWNER/REPO`, then
# `pr create` or `pr ready`. It does not parse shell: a commit message or echo that
# contains those words triggers the check too, which only blocks while the check fails.
# Finding those spellings takes time linear in the length of the command. The hook as a
# whole does not: when the command starts with `cd WORD &&` (or `;`), WORD is parsed with
# shlex.split, which is quadratic in its length (measured: a 400 KB WORD takes about 4 s,
# 800 KB about 14 s).
#
# The checkout checked is the one the command runs in: a leading `cd DIR &&` when DIR is an
# existing directory, else the tool call's cwd. When the cd target cannot be resolved here
# (a shell variable, `$(...)`, backticks, a path that does not exist) the guess would be
# wrong, so both this project's checkout (CLAUDE_PROJECT_DIR) and the cwd are checked and
# the command is blocked if either fails.
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
# Does the command contain `gh`, then options, then `pr create|ready`? As one pattern,
#   (?<![\w-])gh(?:\s+-\S+(?:\s+[^\s-][^\s;&|()]*)?)*\s+pr\s+(?:create|ready)\b
# it is quadratic on `gh -a gh -a ...` (every `gh` backtracks over the rest of the command).
# The same language is decided here per whitespace-separated token instead: `gh` ends a
# token, an option is a token of `-` and more, an option may be followed by one value token
# (not starting with `-`, without `;&|()` after its first character), then the tokens `pr`
# and `create|ready`. ok[i] says whether the tokens from i on read
# `(option [value])* pr create|ready`; it is filled once from the end, so the work is
# linear in the length of the command, and a `gh` inside an option value is just another
# token that cannot hide the `gh` before it.
TOKEN = re.compile(r"\S+")
GH_END = re.compile(r"(?<![\w-])gh\Z")
VALUE = re.compile(r"[^\s-][^\s;&|()]*\Z")
SUBCOMMAND = re.compile(r"(?:create|ready)\b")
def opens_pr(cmd):
    toks = TOKEN.findall(cmd)
    n = len(toks)
    ok = [False] * (n + 2)
    for i in range(n - 1, -1, -1):
        t = toks[i]
        if t == "pr":
            ok[i] = i + 1 < n and SUBCOMMAND.match(toks[i + 1]) is not None
        elif t[0] == "-" and len(t) > 1:
            ok[i] = ok[i + 1] or (i + 1 < n and ok[i + 2] and VALUE.match(toks[i + 1]) is not None)
    return any(ok[i + 1] and GH_END.search(t) for i, t in enumerate(toks))
if not opens_pr(cmd):
    sys.exit(0)
cwd = d.get("cwd") or os.getcwd()
print(d.get("agent_type") or "main")
# A leading `cd DIR &&` (or `;`) selects the checkout; DIR is used only when it is an
# existing directory here.
m = re.match(r"\s*cd\s+(\"[^\"]*\"|\x27[^\x27]*\x27|\S+)\s*(&&|;)", cmd)
target = None
if m:
    try:
        t = os.path.expanduser(shlex.split(m.group(1))[0])
    except (ValueError, IndexError):
        t = ""
    t = t if os.path.isabs(t) else os.path.join(cwd, t)
    if not re.search(r"[$`]", m.group(1)) and os.path.isdir(t):
        target = t
if target:
    print(target)
elif m:
    print(cwd)
    print("@self")   # this project checkout; the hook script expands it
else:
    print(cwd)
') || exit 0
[ -n "$parsed" ] || exit 0
agent=$(printf '%s\n' "$parsed" | sed -n 1p)
dirs=$(printf '%s\n' "$parsed" | sed -n '2,$p')

self_root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Every distinct pg_cuvs checkout among the candidate directories.
roots=""
while IFS= read -r where; do
    [ "$where" = "@self" ] && where="$self_root"
    [ -n "$where" ] || continue
    root=$(git -C "$where" rev-parse --show-toplevel 2>/dev/null) || continue
    [ -f "$root/scripts/prepare-patched-cuvs-source.sh" ] || continue   # not a pg_cuvs checkout
    case $'\n'"$roots" in *$'\n'"$root"$'\n'*) continue;; esac
    roots="$roots$root"$'\n'
done <<<"$dirs"
[ -n "$roots" ] || exit 0

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
failed=0
while IFS= read -r root; do
    [ -n "$root" ] || continue
    check="$root/scripts/check-sdk-patch-path.sh"
    [ -x "$check" ] || check="$self_root/scripts/check-sdk-patch-path.sh"
    [ -x "$check" ] || { echo "[pr-guard] check script not found; not blocking" >&2; continue; }
    if out=$("$check" --root "$root" 2>&1); then
        continue
    fi
    failed=1
    cat >&2 <<MSG
[pr-guard] BLOCKED: the patched cuVS SDK path check failed for $root (caller: $agent).

$(printf '%s\n' "$out" | tail -15)

Run it yourself: scripts/check-sdk-patch-path.sh --root $root
MSG
done <<<"$roots"
[ "$failed" = 1 ] || exit 0
cat >&2 <<'MSG'
Fix the cause before opening the pull request. Genuine exception (for example, no network
for the first fetch of the pinned cuVS source): create a short-lived, attributable bypass —
  mkdir -p .claude/state
  printf 'until=%s\nreason=<why>\n' "$(( $(date +%s) + 1800 ))" > .claude/state/pr-guard-bypass
MSG
exit 2
