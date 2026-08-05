#!/usr/bin/env bash
# gpu-run.sh — one owner, one record, for long-running GPU VM work.
#
# Why this exists. Three signals were being read as "is it running / did it
# finish", and all three lie:
#   * an agent going idle       — means "not generating tokens", not "job done"
#   * `pgrep -f <harness>`      — matches the *asking* command too (self-match)
#   * the launcher's exit code  — nohup returns immediately; the job is untouched
# On top of that, two actors launched the same benchmark against the same --out
# path and one silently killed the other.
#
# The fix is a single queryable fact on the VM instead of three inferences:
#   /tmp/pgcuvs-run.json   { name, owner, cmd, pid, state, started_at, ... }
# and an flock the job holds for its whole lifetime, so a second launch fails
# fast and *names who holds it* instead of colliding.
#
# Usage
#   gpu-run.sh status                     # what is running / how the last run ended
#   gpu-run.sh run <name> -- <command…>   # preflight, take the lock, run detached
#   gpu-run.sh log  [n]                   # tail the current/last run's log
#   gpu-run.sh log  -f                    # follow it live (Ctrl-C to detach)
#
# The command runs on the VM under the bench env (PGHOST/PATH/LD_LIBRARY_PATH),
# cwd = ~/pg_cuvs.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STATE=/tmp/pgcuvs-run.json
LOCK=/tmp/pgcuvs-run.lock

# #169: parse, do not source. gpu.conf is `-include`d by the Makefile, so
# `VM_SSH_HOST = host` (spaces around `=`) is legal there and fatal here —
# `.` runs the key as a command, 127, and `set -e` kills this script before it
# can launch anything. Same fix as gpu-preflight.sh.
conf_get() {
    local key="$1" f v
    for f in "$REPO_ROOT/gpu.conf" "$REPO_ROOT/.env.gpu"; do
        [ -f "$f" ] || continue
        v=$(sed -n "s/^[[:space:]]*$key[[:space:]]*=[[:space:]]*//p" "$f" | tail -1)
        v=${v%%#*}
        v="$(printf '%s' "$v" | tr -d '"'\''' | xargs 2>/dev/null || true)"
        [ -n "$v" ] && { printf '%s' "$v"; return 0; }
    done
    return 0
}

VM_SSH_HOST="${VM_SSH_HOST:-$(conf_get VM_SSH_HOST)}"
[ -n "$VM_SSH_HOST" ] || VM_SSH_HOST="$(conf_get GCP_VM)"
[ -n "$VM_SSH_HOST" ] || { echo "gpu-run: VM_SSH_HOST unset (gpu.conf)" >&2; exit 1; }

OWNER="${PGCUVS_RUN_OWNER:-${USER:-unknown}}"
vm() { ssh -o BatchMode=yes "$VM_SSH_HOST" "$@"; }

case "${1:-status}" in
status)
    vm "cat $STATE 2>/dev/null || echo '{\"state\":\"none\"}'" | python3 -c '
import json, sys, time
d = json.load(sys.stdin)
st = d.get("state", "none")
if st == "none":
    print("[gpu-run] no run recorded")
    sys.exit(0)
g = d.get
print("[gpu-run] %s  state=%s  owner=%s" % (g("name"), st, g("owner")))
print("          started %s  pid=%s" % (g("started_at"), g("pid")))
if st == "running":
    print("          elapsed %ds" % (int(time.time()) - int(g("started_epoch", 0))))
else:
    print("          ended   %s  exit=%s" % (g("ended_at"), g("exit_code")))
print("          log %s" % g("log"))
print("          cmd %s" % g("cmd"))
'
    ;;
log)
    # `log -f` follows the run's log live (ssh + tail -F, Ctrl-C to detach) so a
    # run's progress can be watched as it happens; `log [n]` keeps its old
    # meaning (print the last n lines and return).
    if [ "${2:-}" = "-f" ]; then
        vm -t "l=\$(python3 -c \"import json;print(json.load(open('$STATE'))['log'])\" 2>/dev/null); [ -n \"\$l\" ] && tail -n 40 -F \"\$l\" || echo '(no log)'"
    else
        n="${2:-40}"
        vm "l=\$(python3 -c \"import json;print(json.load(open('$STATE'))['log'])\" 2>/dev/null); [ -n \"\$l\" ] && tail -n $n \"\$l\" || echo '(no log)'"
    fi
    ;;
run)
    shift
    name="${1:-run}"; shift
    [ "${1:-}" = "--" ] && shift
    [ $# -gt 0 ] || { echo "gpu-run: no command given" >&2; exit 1; }
    cmd="$*"

    # Stale artifacts make a measurement meaningless; refuse before taking the lock.
    "$REPO_ROOT/infra/scripts/gpu-preflight.sh" || {
        echo "gpu-run: preflight failed — fix the above, or re-run with PGCUVS_SKIP_PREFLIGHT=1" >&2
        [ "${PGCUVS_SKIP_PREFLIGHT:-0}" = "1" ] || exit 1
    }

    b64=$(printf '%s' "$cmd" | base64 | tr -d '\n')
    vm "NAME='$name' OWNER='$OWNER' CMD_B64='$b64' bash -s" <<'REMOTE'
set -uo pipefail
STATE=/tmp/pgcuvs-run.json
LOCK=/tmp/pgcuvs-run.lock
LOG=/tmp/pgcuvs-$NAME.log
CMD=$(printf '%s' "$CMD_B64" | base64 -d)

# Fail fast and say who holds it, rather than racing.
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "gpu-run: VM is busy — another run holds the lock:" >&2
    cat "$STATE" 2>/dev/null >&2
    exit 75          # EX_TEMPFAIL
fi
exec 9>&-            # the detached child re-takes it; parent must not hold it

# The child runs from a file, not a nested `bash -c '…'`: the quoting needed to
# splice variables through two layers of shell silently dropped them, the child
# died on an unset $STATE, and both the lock and the state record vanished —
# exactly the failure this script exists to prevent.
RUNNER=/tmp/pgcuvs-runner-$$.sh
cat >"$RUNNER" <<'CHILD'
#!/usr/bin/env bash
exec 9>"$LOCK"
flock -n 9 || exit 75
python3 - <<PY
import json, os, time, datetime
json.dump({"name": os.environ["NAME"], "owner": os.environ["OWNER"],
           "cmd": os.environ["CMD"], "pid": os.getpid(), "state": "running",
           "started_at": datetime.datetime.now().astimezone().isoformat(),
           "started_epoch": int(time.time()), "log": os.environ["LOG"]},
          open(os.environ["STATE"], "w"))
PY
cd "$HOME/pg_cuvs" || exit 70
export PGHOST=/var/run/postgresql
# PG16 bindir first: the cuvs_bench env ships its own pg_config, and PGXS asks
# pg_config where the binaries live. With conda first, `make installcheck` was
# sent to conda's bindir — which has no psql — and died as "psql not found".
# python3 still resolves to cuvs_bench (PG's bindir has none), so bench scripts
# keep the env they need.
export PATH=/usr/lib/postgresql/16/bin:/opt/miniforge3/envs/cuvs_bench/bin:$PATH
export LD_LIBRARY_PATH=/opt/miniforge3/envs/cuvs_dev/lib
bash -c "$CMD" >"$LOG" 2>&1
rc=$?
RC=$rc python3 - <<PY
import json, os, datetime
p = os.environ["STATE"]; d = json.load(open(p))
rc = int(os.environ["RC"])
d.update(state=("done" if rc == 0 else "failed"), exit_code=rc,
         ended_at=datetime.datetime.now().astimezone().isoformat())
json.dump(d, open(p, "w"))
PY
rm -f "$0"
CHILD
chmod +x "$RUNNER"

export NAME OWNER CMD STATE LOG LOCK
setsid nohup "$RUNNER" >/dev/null 2>&1 &

# Confirm the child actually took the lock and wrote its record; a launcher that
# reports success without checking is the bug we are removing. Match on the run
# *name*, not state=="running": a short command can already be "done" by the
# time we poll, and treating that as a failed start is itself a false negative.
ok=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if [ -f "$STATE" ] && grep -q "\"name\": *\"$NAME\"" "$STATE"; then ok=1; break; fi
    sleep 0.5
done
if [ "$ok" != "1" ]; then
    echo "gpu-run: child never recorded a run for '$NAME'; log $LOG" >&2
    exit 1
fi
echo "gpu-run: started '$NAME' (log $LOG)"
REMOTE
    echo "gpu-run: poll with  infra/scripts/gpu-run.sh status"
    ;;
*)
    sed -n '2,30p' "$0"; exit 1;;
esac
