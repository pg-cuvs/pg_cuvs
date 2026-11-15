#!/usr/bin/env bash
# Check that a release tag is consistent with the tree it points to.
#
#   scripts/release-check.sh vX.Y.Z [<commit>]
#
# <commit> defaults to HEAD. Run it from inside a clone that has the remote branches
# (actions/checkout with fetch-depth: 0). Every check runs and every failure is
# printed; the exit status is 1 if there was any.
#
#   * the tag has the form vX.Y.Z
#   * pg_cuvs.control default_version, Makefile EXTVERSION and PG_CUVS_VERSION
#     (src/cuvs_version.h) all equal X.Y.Z
#   * sql/pg_cuvs--X.Y.Z.sql exists
#   * the branch release/vX.Y.Z exists on $REMOTE (default origin) and contains
#     the commit, so a tag cannot be pushed from a topic branch or from main
#   * CHANGELOG.md has a non-empty "## [X.Y.Z]" section
set -uo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: release-check.sh vX.Y.Z [<commit>]" >&2
    exit 2
fi
tag="$1"
commit="${2:-HEAD}"
remote="${REMOTE:-origin}"
errors=0

fail() {
    printf '[FAIL] %s\n' "$1" >&2
    errors=$((errors + 1))
}

if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    fail "tag '$tag' is not of the form vX.Y.Z"
    exit 1
fi
version="${tag#v}"
# Work in the repository of the current directory; the sibling script is found
# next to this one.
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git rev-parse --show-toplevel)" || { fail "not inside a git work tree"; exit 1; }
cd "$repo_root" || exit 1

sha="$(git rev-parse --verify --quiet "${commit}^{commit}")" || { fail "cannot resolve commit '$commit'"; exit 1; }

# Read the files as they are at <commit>, not as they are in the work tree.
show() { git show "$sha:$1" 2>/dev/null; }

control_version="$(show pg_cuvs.control | sed -n "s/^default_version = '\([^']*\)'.*/\1/p" | head -n 1)"
make_version="$(show Makefile | sed -n 's/^EXTVERSION[[:space:]]*=[[:space:]]*//p' | head -n 1)"
header_version="$(show src/cuvs_version.h | sed -n 's/^#define PG_CUVS_VERSION "\([^"]*\)".*/\1/p' | head -n 1)"

[[ "$control_version" == "$version" ]] || fail "pg_cuvs.control default_version is '${control_version:-?}', the tag says $version"
[[ "$make_version" == "$version" ]] || fail "Makefile EXTVERSION is '${make_version:-?}', the tag says $version"
[[ "$header_version" == "$version" ]] || fail "PG_CUVS_VERSION in src/cuvs_version.h is '${header_version:-?}', the tag says $version"

git cat-file -e "$sha:sql/pg_cuvs--${version}.sql" 2>/dev/null || fail "sql/pg_cuvs--${version}.sql does not exist at ${sha:0:12}"

branch="release/${tag}"
if ! git rev-parse --verify --quiet "refs/remotes/${remote}/${branch}" >/dev/null; then
    fail "branch ${remote}/${branch} does not exist: cut it from main before tagging"
elif ! git merge-base --is-ancestor "$sha" "refs/remotes/${remote}/${branch}"; then
    fail "commit ${sha:0:12} is not on ${remote}/${branch}: tag the last commit of the release branch"
fi

changelog_tmp="$(mktemp)"
trap 'rm -f "$changelog_tmp"' EXIT
if show CHANGELOG.md > "$changelog_tmp"; then
    CHANGELOG="$changelog_tmp" bash "$scripts_dir/release-notes.sh" "$version" body >/dev/null 2>&1 \
        || fail "CHANGELOG.md has no non-empty section '## [$version]' at ${sha:0:12}"
else
    fail "CHANGELOG.md does not exist at ${sha:0:12}"
fi

if (( errors > 0 )); then
    printf '[FAIL] release check for %s: %d failure(s)\n' "$tag" "$errors" >&2
    exit 1
fi
printf '[OK] release check for %s at %s\n' "$tag" "${sha:0:12}"
