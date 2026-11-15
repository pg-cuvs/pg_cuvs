#!/usr/bin/env bash
# Tests for scripts/release-check.sh and scripts/release-notes.sh, which the
# release workflow (.github/workflows/release.yml) runs on a tag push. No network,
# no GitHub: each case builds a throwaway repository with a bare "origin", the
# files the scripts read, and a release branch, then runs the real scripts.
#
#   bash test/release/test_release_scripts.sh
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scripts="$(cd "$here/../../scripts" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

export GIT_AUTHOR_NAME=fixture GIT_AUTHOR_EMAIL=fixture@example.invalid
export GIT_COMMITTER_NAME=fixture GIT_COMMITTER_EMAIL=fixture@example.invalid
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null

fails=0
pass() { printf 'PASS %s\n' "$1"; }
flunk() { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }

# expect <name> <exit status> <pattern in output or ""> -- <command...>
expect() {
    local name="$1" want_rc="$2" pattern="$3"
    shift 4
    local out rc
    out="$("$@" 2>&1)"
    rc=$?
    if [[ $rc -ne $want_rc ]]; then
        flunk "$name (exit $rc, wanted $want_rc): $out"
    elif [[ -n "$pattern" ]] && ! grep -Fq -- "$pattern" <<<"$out"; then
        flunk "$name (output lacks '$pattern'): $out"
    else
        pass "$name"
    fi
}

write_tree() { # <dir> <version> : the files release-check.sh reads
    local d="$1" v="$2"
    mkdir -p "$d/sql" "$d/src"
    printf "default_version = '%s'\nrelocatable = false\n" "$v" > "$d/pg_cuvs.control"
    printf 'EXTENSION      = pg_cuvs\nEXTVERSION     = %s\n' "$v" > "$d/Makefile"
    printf '#ifndef PG_CUVS_VERSION\n#define PG_CUVS_VERSION "%s"\n#endif\n' "$v" > "$d/src/cuvs_version.h"
    : > "$d/sql/pg_cuvs--${v}.sql"
    cat > "$d/CHANGELOG.md" <<CL
# Changelog

## [Unreleased]

### Fixed
- Something after the release.

## [${v}] - 2026-11-20

Fixture release summary.

### Added
- A thing.

## [0.0.9] - 2026-01-01

Older release.
CL
}

# new_fixture <name> <version> : bare origin + clone "work" with main and
# release/v<version> at the same commit; prints the work directory.
new_fixture() {
    local base="$tmp/$1" v="$2"
    git init -q --bare -b main "$base/origin.git"
    git clone -q "$base/origin.git" "$base/work" 2>/dev/null
    git -C "$base/work" checkout -q -b main
    write_tree "$base/work" "$v"
    git -C "$base/work" add -A
    git -C "$base/work" commit -q -m "cut"
    git -C "$base/work" push -q origin main
    git -C "$base/work" push -q origin "main:refs/heads/release/v${v}"
    git -C "$base/work" fetch -q origin
    printf '%s\n' "$base/work"
}

in_repo() { ( cd "$1" && shift && "$@" ); }

# ---------------------------------------------------------------- release-check
w="$(new_fixture ok 0.9.1)"
expect "check: consistent tag on the release branch passes" 0 "[OK] release check for v0.9.1" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1
expect "check: tag name without the v prefix is refused" 1 "is not of the form vX.Y.Z" -- \
    in_repo "$w" bash "$scripts/release-check.sh" 0.9.1
expect "check: tag with a suffix is refused" 1 "is not of the form vX.Y.Z" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1-rc1
expect "check: a tag whose version is not the control version is refused" 1 "default_version is '0.9.1', the tag says 0.9.2" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.2

w="$(new_fixture no-branch 0.9.1)"
git -C "$w" push -q origin --delete release/v0.9.1 && git -C "$w" fetch -q --prune origin
expect "check: a missing release branch is refused" 1 "origin/release/v0.9.1 does not exist" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture off-branch 0.9.1)"
echo "later work" > "$w/later.txt"
git -C "$w" add later.txt && git -C "$w" commit -q -m "main moves on" && git -C "$w" push -q origin main
expect "check: a commit that is on main but not on the release branch is refused" 1 "is not on origin/release/v0.9.1" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1
expect "check: the release-branch commit itself still passes" 0 "[OK]" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1 origin/release/v0.9.1

w="$(new_fixture ahead 0.9.1)"
echo "fix" > "$w/fix.txt"
git -C "$w" add fix.txt && git -C "$w" commit -q -m "cherry-picked fix"
git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: a commit below the tip of the release branch passes" 0 "[OK]" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1 HEAD~1

w="$(new_fixture bad-control 0.9.1)"
sed -i.bak "s/0\.9\.1/0.9.0/" "$w/pg_cuvs.control" && rm -f "$w/pg_cuvs.control.bak"
git -C "$w" commit -q -am "stale control" && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: control version behind the tag is refused" 1 "default_version is '0.9.0'" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture bad-make 0.9.1)"
sed -i.bak "s/0\.9\.1/0.9.0/" "$w/Makefile" && rm -f "$w/Makefile.bak"
git -C "$w" commit -q -am "stale make" && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: Makefile EXTVERSION behind the tag is refused" 1 "Makefile EXTVERSION is '0.9.0'" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture bad-header 0.9.1)"
sed -i.bak "s/0\.9\.1/0.9.0/" "$w/src/cuvs_version.h" && rm -f "$w/src/cuvs_version.h.bak"
git -C "$w" commit -q -am "stale header" && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: PG_CUVS_VERSION behind the tag is refused" 1 "PG_CUVS_VERSION in src/cuvs_version.h is '0.9.0'" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture no-sql 0.9.1)"
git -C "$w" rm -q sql/pg_cuvs--0.9.1.sql && git -C "$w" commit -q -m "no install script" \
    && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: a missing install script is refused" 1 "sql/pg_cuvs--0.9.1.sql does not exist" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture no-section 0.9.1)"
sed -i.bak 's/^## \[0\.9\.1\] - 2026-11-20$/## [0.9.2] - 2026-11-20/' "$w/CHANGELOG.md" && rm -f "$w/CHANGELOG.md.bak"
git -C "$w" commit -q -am "wrong changelog section" && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: a CHANGELOG without the section is refused" 1 "no non-empty section '## [0.9.1]'" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture empty-section 0.9.1)"
printf '# Changelog\n\n## [Unreleased]\n\n## [0.9.1] - 2026-11-20\n\n## [0.0.9] - 2026-01-01\n\nOlder.\n' > "$w/CHANGELOG.md"
git -C "$w" commit -q -am "empty section" && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: an empty CHANGELOG section is refused" 1 "no non-empty section '## [0.9.1]'" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture no-changelog 0.9.1)"
git -C "$w" rm -q CHANGELOG.md && git -C "$w" commit -q -m "no changelog" \
    && git -C "$w" push -q origin main:refs/heads/release/v0.9.1 && git -C "$w" fetch -q origin
expect "check: a missing CHANGELOG.md is refused" 1 "CHANGELOG.md does not exist" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

w="$(new_fixture dirty-tree 0.9.1)"
sed -i.bak "s/0\.9\.1/0.9.7/" "$w/pg_cuvs.control" && rm -f "$w/pg_cuvs.control.bak"
expect "check: reads the committed files, not an edited work tree" 0 "[OK]" -- \
    in_repo "$w" bash "$scripts/release-check.sh" v0.9.1

# --------------------------------------------------------------- release-notes
cl="$tmp/changelog.md"
cat > "$cl" <<'CL'
# Changelog

Intro text.

## [Unreleased]

### Added
- Not in a release yet.

## [0.9.10] - 2026-12-01

Ten.

## [0.9.1] - 2026-11-20

Fixture release summary.

### Added
- A thing.

### Fixed
- Another thing measured on one VM (A100, 3-run median).


## [0.0.9] - 2026-01-01

Older release.

[Unreleased]: https://example.invalid/compare/v0.9.1...HEAD
CL
export CHANGELOG="$cl"
body="$(bash "$scripts/release-notes.sh" 0.9.1 body)"
if [[ "$body" == "$(printf 'Fixture release summary.\n\n### Added\n- A thing.\n\n### Fixed\n- Another thing measured on one VM (A100, 3-run median).')" ]]; then
    pass "notes: body is the section text without heading, neighbours or surrounding blank lines"
else
    flunk "notes: body differs: $body"
fi
expect "notes: summary is the first prose line" 0 "Fixture release summary." -- bash "$scripts/release-notes.sh" 0.9.1 summary
expect "notes: title joins tag and summary with an em dash" 0 "v0.9.1 — Fixture release summary." -- bash "$scripts/release-notes.sh" 0.9.1 title
if bash "$scripts/release-notes.sh" 0.9.1 body | grep -Fq "Ten."; then flunk "notes: [0.9.10] text leaked into 0.9.1"; else pass "notes: [0.9.10] text does not leak into 0.9.1"; fi
if bash "$scripts/release-notes.sh" 0.0.9 body | grep -Fq "example.invalid"; then flunk "notes: link reference leaked into the last section"; else pass "notes: link references after the last section are not part of it"; fi
expect "notes: the last section is read to its end" 0 "Older release." -- bash "$scripts/release-notes.sh" 0.0.9 body
expect "notes: a version with no section is an error" 1 "no non-empty section" -- bash "$scripts/release-notes.sh" 9.9.9 body
expect "notes: a version that is not X.Y.Z is bad usage" 2 "is not X.Y.Z" -- bash "$scripts/release-notes.sh" v0.9.1 body
expect "notes: an unknown part is bad usage" 2 "usage" -- bash "$scripts/release-notes.sh" 0.9.1 header
printf '# Changelog\n\n## [1.0.0] - 2027-01-01\n\n### Added\n- Only a list, no prose.\n' > "$tmp/nosummary.md"
CHANGELOG="$tmp/nosummary.md" expect "notes: a section without prose has a bare title" 0 "v1.0.0" -- bash "$scripts/release-notes.sh" 1.0.0 title
if [[ "$(CHANGELOG="$tmp/nosummary.md" bash "$scripts/release-notes.sh" 1.0.0 title)" == "v1.0.0" ]]; then pass "notes: bare title is exactly vX.Y.Z"; else flunk "notes: bare title is not exactly v1.0.0"; fi
expect "notes: a missing CHANGELOG is an error" 1 "not found" -- env CHANGELOG="$tmp/absent.md" bash "$scripts/release-notes.sh" 0.9.1 body

if (( fails > 0 )); then
    printf 'test_release_scripts: %d FAILED\n' "$fails" >&2
    exit 1
fi
echo "test_release_scripts: all passed"
