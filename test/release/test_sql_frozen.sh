#!/usr/bin/env bash
# Tests for scripts/check-sql-frozen.sh. Each case builds a throwaway repository
# with a base commit and one change, then runs the real script on base..head.
#
#   bash test/release/test_sql_frozen.sh
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$(cd "$here/../../scripts" && pwd)/check-sql-frozen.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export GIT_AUTHOR_NAME=fixture GIT_AUTHOR_EMAIL=fixture@example.invalid
export GIT_COMMITTER_NAME=fixture GIT_COMMITTER_EMAIL=fixture@example.invalid
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null

fails=0
pass() { printf 'PASS %s\n' "$1"; }
flunk() { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }

# fixture <name> : development version 0.9.1, scripts of 0.9.0 (frozen) and 0.9.1
fixture() {
    local d="$tmp/$1"
    git init -q -b main "$d"
    mkdir -p "$d/sql"
    printf "default_version = '0.9.1'\n" > "$d/pg_cuvs.control"
    printf -- '-- 0.9.0 install\n' > "$d/sql/pg_cuvs--0.9.0.sql"
    printf -- '-- 0.8.2 to 0.9.0\n' > "$d/sql/pg_cuvs--0.8.2--0.9.0.sql"
    printf -- '-- 0.9.1 install\n' > "$d/sql/pg_cuvs--0.9.1.sql"
    printf -- '-- 0.9.0 to 0.9.1\n' > "$d/sql/pg_cuvs--0.9.0--0.9.1.sql"
    git -C "$d" add -A && git -C "$d" commit -q -m base
    printf '%s\n' "$d"
}

# run <name> <expected rc> <pattern> <script args...> -- run in $d
verdict() { # <name> <repo> <want rc> <pattern> <extra args...>
    local name="$1" d="$2" want="$3" pattern="$4"
    shift 4
    local out rc
    out="$(cd "$d" && bash "$script" "$@" 2>&1)"
    rc=$?
    if [[ $rc -ne $want ]]; then
        flunk "$name (exit $rc, wanted $want): $out"
    elif [[ -n "$pattern" ]] && ! grep -Fq -- "$pattern" <<<"$out"; then
        flunk "$name (output lacks '$pattern'): $out"
    else
        pass "$name"
    fi
}

d="$(fixture none)"
git -C "$d" commit -q --allow-empty -m "no sql change"
verdict "no SQL change passes" "$d" 0 "[OK]" HEAD~1 HEAD

d="$(fixture edit-current)"
echo "-- edited" >> "$d/sql/pg_cuvs--0.9.1.sql"; echo "-- edited" >> "$d/sql/pg_cuvs--0.9.0--0.9.1.sql"
git -C "$d" commit -q -am "edit the development version"
verdict "editing the development version passes" "$d" 0 "[OK]" HEAD~1 HEAD

d="$(fixture edit-frozen-install)"
echo "-- edited" >> "$d/sql/pg_cuvs--0.9.0.sql"
git -C "$d" commit -q -am "edit a frozen install script"
verdict "editing a frozen install script fails" "$d" 1 "M sql/pg_cuvs--0.9.0.sql: version 0.9.0 is frozen" HEAD~1 HEAD

d="$(fixture edit-frozen-upgrade)"
echo "-- edited" >> "$d/sql/pg_cuvs--0.8.2--0.9.0.sql"
git -C "$d" commit -q -am "edit a frozen upgrade script"
verdict "editing a frozen upgrade script fails" "$d" 1 "sql/pg_cuvs--0.8.2--0.9.0.sql: version 0.9.0 is frozen" HEAD~1 HEAD

d="$(fixture delete-frozen)"
git -C "$d" rm -q sql/pg_cuvs--0.9.0.sql && git -C "$d" commit -q -m "delete"
verdict "deleting a frozen script fails" "$d" 1 "D sql/pg_cuvs--0.9.0.sql" HEAD~1 HEAD

d="$(fixture rename-frozen)"
git -C "$d" mv sql/pg_cuvs--0.9.0.sql sql/pg_cuvs--0.9.0-old.sql && git -C "$d" commit -q -m "rename"
verdict "renaming a frozen script fails" "$d" 1 "D sql/pg_cuvs--0.9.0.sql" HEAD~1 HEAD

d="$(fixture bump)"
printf -- '-- 0.9.1 install\n' > "$d/sql/pg_cuvs--0.9.2.sql"; printf -- '-- 0.9.1 to 0.9.2\n' > "$d/sql/pg_cuvs--0.9.1--0.9.2.sql"
printf "default_version = '0.9.2'\n" > "$d/pg_cuvs.control"
git -C "$d" add -A && git -C "$d" commit -q -m "bump to 0.9.2"
verdict "a bump that only adds scripts passes" "$d" 0 "development version 0.9.2" HEAD~1 HEAD

d="$(fixture bump-and-edit)"
printf -- '-- 0.9.1 install\n' > "$d/sql/pg_cuvs--0.9.2.sql"; printf -- '-- 0.9.1 to 0.9.2\n' > "$d/sql/pg_cuvs--0.9.1--0.9.2.sql"
printf "default_version = '0.9.2'\n" > "$d/pg_cuvs.control"; echo "-- last tweak" >> "$d/sql/pg_cuvs--0.9.1.sql"
git -C "$d" add -A && git -C "$d" commit -q -m "bump to 0.9.2 and edit 0.9.1"
verdict "a bump that also edits the version it freezes fails" "$d" 1 "M sql/pg_cuvs--0.9.1.sql: version 0.9.1 is frozen" HEAD~1 HEAD

d="$(fixture ten)"
printf -- '-- 0.9.9 install\n' > "$d/sql/pg_cuvs--0.9.9.sql"; printf "default_version = '0.9.9'\n" > "$d/pg_cuvs.control"
git -C "$d" add -A && git -C "$d" commit -q -m "bump to 0.9.9"
printf -- '-- 0.9.10 install\n' > "$d/sql/pg_cuvs--0.9.10.sql"; printf "default_version = '0.9.10'\n" > "$d/pg_cuvs.control"
git -C "$d" add -A && git -C "$d" commit -q -m "bump to 0.9.10"
echo "-- edited" >> "$d/sql/pg_cuvs--0.9.9.sql"; git -C "$d" commit -q -am "edit 0.9.9 while 0.9.10 is in development"
verdict "version order is numeric (0.9.9 is frozen below 0.9.10)" "$d" 1 "version 0.9.9 is frozen" HEAD~1 HEAD

d="$(fixture add-old)"
printf -- '-- 0.9.0 to 0.9.0 gap filler\n' > "$d/sql/pg_cuvs--0.8.0--0.9.0.sql"
git -C "$d" add -A && git -C "$d" commit -q -m "add a missing upgrade script of a frozen version"
verdict "adding a script of a frozen version passes" "$d" 0 "[OK]" HEAD~1 HEAD

d="$(fixture other-files)"
echo "x" > "$d/README.md"; echo "-- note" > "$d/sql/notes.txt"
git -C "$d" add -A && git -C "$d" commit -q -m "other files"
verdict "files that are not versioned scripts are ignored" "$d" 0 "[OK]" HEAD~1 HEAD

d="$(fixture release-edit)"
echo "-- edited" >> "$d/sql/pg_cuvs--0.9.1.sql"
git -C "$d" commit -q -am "edit on a release branch"
verdict "on a release branch even the current version is closed" "$d" 1 "release/v0.9.1 takes no change to SQL scripts" HEAD~1 HEAD --release-branch 0.9.1

d="$(fixture release-add)"
printf -- '-- new\n' > "$d/sql/pg_cuvs--0.9.1--0.9.2.sql"
git -C "$d" add -A && git -C "$d" commit -q -m "add on a release branch"
verdict "on a release branch adding a script fails too" "$d" 1 "A sql/pg_cuvs--0.9.1--0.9.2.sql" HEAD~1 HEAD --release-branch 0.9.1

d="$(fixture release-add-own)"
git -C "$d" commit -q --allow-empty -m "tag point"
printf -- '-- 0.9.2 install\n' > "$d/sql/pg_cuvs--0.9.2.sql"; printf -- '-- 0.9.1 to 0.9.2\n' > "$d/sql/pg_cuvs--0.9.1--0.9.2.sql"
printf "default_version = '0.9.2'\n" > "$d/pg_cuvs.control"
git -C "$d" add -A && git -C "$d" commit -q -m "raise the version on a hotfix branch"
verdict "on release/v0.9.2 adding the scripts of 0.9.2 passes" "$d" 0 "[OK]" HEAD~1 HEAD --release-branch 0.9.2
verdict "on release/v0.9.1 the same addition fails" "$d" 1 "release/v0.9.1 takes no change" HEAD~1 HEAD --release-branch 0.9.1

d="$(fixture release-edit-own)"
git -C "$d" commit -q --allow-empty -m "tag point"
echo "-- edited" >> "$d/sql/pg_cuvs--0.9.1.sql"; git -C "$d" commit -q -am "edit the scripts of the release"
verdict "on release/v0.9.1 editing its own scripts fails" "$d" 1 "M sql/pg_cuvs--0.9.1.sql" HEAD~1 HEAD --release-branch 0.9.1

d="$(fixture release-none)"
echo "fix" > "$d/fix.c"; git -C "$d" add -A && git -C "$d" commit -q -m "C fix on a release branch"
verdict "on a release branch a change without SQL passes" "$d" 0 "[OK]" HEAD~1 HEAD --release-branch 0.9.1

d="$(fixture bad-ref)"
verdict "an unknown base is an error" "$d" 1 "cannot resolve base" no-such-ref HEAD
verdict "missing arguments are bad usage" "$d" 2 "usage"
verdict "--release-branch without a version is bad usage" "$d" 2 "needs a version" HEAD~0 HEAD --release-branch
verdict "--release-branch with v-prefixed version is bad usage" "$d" 2 "needs a version" HEAD HEAD --release-branch v0.9.1

if (( fails > 0 )); then
    printf 'test_sql_frozen: %d FAILED\n' "$fails" >&2
    exit 1
fi
echo "test_sql_frozen: all passed"
