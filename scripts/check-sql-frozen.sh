#!/usr/bin/env bash
# Fail when a change edits, renames or deletes an SQL script of a version that
# is no longer in development.
#
#   scripts/check-sql-frozen.sh <base-commit> [<head-commit>] [--release-branch X.Y.Z]
#
# The version in development is default_version of pg_cuvs.control at <head-commit>
# (default HEAD). sql/pg_cuvs--A.sql belongs to the version A and sql/pg_cuvs--A--B.sql
# to the larger of A and B: an upgrade is part of the version it lands on, a downgrade
# (A > B) is made and checked together with the version it leaves, so it can still be
# fixed while A is in development and is frozen with A. Between <base-commit> and
# <head-commit>:
#
#   * a script of a version below the development version must not be modified,
#     deleted or renamed; a new script of any version may be added;
#   * with --release-branch X.Y.Z (a pull request into release/vX.Y.Z) no SQL script
#     may be touched, except that the scripts of X.Y.Z itself may be added: that is
#     the version-raising commit of a hotfix branch. A fix that needs SQL goes into
#     the next version on main.
#
# A version is frozen by the commit that moves default_version past it, so a last
# edit of the old scripts must be merged before that commit, not with it.
set -uo pipefail

release_version=""
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release-branch)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                echo "usage: --release-branch needs a version X.Y.Z" >&2
                exit 2
            fi
            release_version="$2"
            shift 2
            ;;
        *) args+=("$1"); shift ;;
    esac
done
if [[ ${#args[@]} -lt 1 || ${#args[@]} -gt 2 ]]; then
    echo "usage: check-sql-frozen.sh <base-commit> [<head-commit>] [--release-branch X.Y.Z]" >&2
    exit 2
fi
base="${args[0]}"
head="${args[1]:-HEAD}"

repo_root="$(git rev-parse --show-toplevel)" || { echo "[FAIL] not inside a git work tree" >&2; exit 1; }
cd "$repo_root" || exit 1

git rev-parse --verify --quiet "${base}^{commit}" >/dev/null || { echo "[FAIL] cannot resolve base '$base'" >&2; exit 1; }
git rev-parse --verify --quiet "${head}^{commit}" >/dev/null || { echo "[FAIL] cannot resolve head '$head'" >&2; exit 1; }

current="$(git show "${head}:pg_cuvs.control" 2>/dev/null | sed -n "s/^default_version = '\([^']*\)'.*/\1/p" | head -n 1)"
if [[ -z "$current" ]]; then
    echo "[FAIL] cannot read default_version of pg_cuvs.control at $head" >&2
    exit 1
fi

older_than() { # true when version $1 < $2
    [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" == "$1" ]]
}

errors=0
while IFS=$'\t' read -r status path; do
    [[ -n "$path" ]] || continue
    name="${path##*/}"
    if [[ ! "$name" =~ ^pg_cuvs--([0-9]+\.[0-9]+\.[0-9]+)(--([0-9]+\.[0-9]+\.[0-9]+))?\.sql$ ]]; then
        continue
    fi
    target="${BASH_REMATCH[3]:-${BASH_REMATCH[1]}}"
    if [[ -n "${BASH_REMATCH[3]}" ]] && older_than "$target" "${BASH_REMATCH[1]}"; then
        target="${BASH_REMATCH[1]}"	# a downgrade belongs to the version it leaves
    fi
    if [[ -n "$release_version" ]]; then
        if [[ "$status" == "A" && "$target" == "$release_version" ]]; then
            continue
        fi
        printf '[FAIL] %s %s: release/v%s takes no change to SQL scripts but the addition of the scripts of %s; put it in the next version on main\n' \
            "$status" "$path" "$release_version" "$release_version" >&2
        errors=$((errors + 1))
    elif [[ "$status" != "A" ]] && older_than "$target" "$current"; then
        printf '[FAIL] %s %s: version %s is frozen (development version is %s); fix forward in the scripts of %s\n' \
            "$status" "$path" "$target" "$current" "$current" >&2
        errors=$((errors + 1))
    fi
done < <(git diff --name-status --no-renames "$base" "$head" -- 'sql/pg_cuvs--*.sql')

if (( errors > 0 )); then
    printf '[FAIL] frozen SQL scripts: %d violation(s)\n' "$errors" >&2
    exit 1
fi
printf '[OK] no frozen SQL script changed between %s and %s (development version %s)\n' "${base:0:12}" "${head:0:12}" "$current"
