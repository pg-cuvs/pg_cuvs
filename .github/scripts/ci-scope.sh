#!/usr/bin/env bash
# Decide whether a pull request changes documentation only. ci.yml skips the
# Tier 1 and page-format jobs for such a pull request; Unit tests (CPU), which
# runs the documentation contract check, always runs.
#
#   ci-scope.sh classify          read changed paths, one per line, on stdin
#   ci-scope.sh diff BASE HEAD    classify `git diff --no-renames BASE HEAD`
#   ci-scope.sh self-test         check both against the table below
#
# classify and diff print `true` or `false` and exit 0. Anything they cannot
# classify prints `false`, so the heavy jobs run: an empty list, a git error,
# a path git had to quote, and a rename whose old or new side is outside the
# documentation scope (--no-renames lists both sides).
#
# Documentation scope: Markdown files at the top level, under docs/ and under
# design/. Not in scope: every other file type, Markdown anywhere else, and
# documentation that a Tier 1 or page-format step reads. Those steps read
# docs/reference.md (test/unit/run_manual_checks.py runs check_i3b_docs.py,
# check_lw_docs.py, check_i3e_sql.py, mutate_i3b_docs.sh, test_lw_mutations.py
# and test_i3e_mutations.py, which parse it) and infra/cuvs-patches/README.md
# (test/native/hnsw-sdk/test_upper_level_precision.py, outside the scope by
# directory). A new test that reads a file in the scope adds it to EXCLUDED.
set -uo pipefail

EXCLUDED=(docs/reference.md)

in_scope() {
    local path=$1 excluded
    for excluded in "${EXCLUDED[@]}"; do
        [[ $path == "$excluded" ]] && return 1
    done
    # git quotes names with unusual bytes; such a name is not classified.
    [[ $path =~ ^[A-Za-z0-9._/-]+$ ]] || return 1
    case "$path" in
        docs/*.md|design/*.md) return 0 ;;
        */*) return 1 ;;
        *.md) return 0 ;;
    esac
    return 1
}

classify() {
    local path count=0
    while IFS= read -r path || [[ -n $path ]]; do
        [[ -z $path ]] && continue
        count=$((count + 1))
        if ! in_scope "$path"; then
            echo false
            return 0
        fi
    done
    if ((count == 0)); then echo false; else echo true; fi
}

diff_scope() {
    local list
    if ! list=$(git -c core.quotePath=true diff --name-only --no-renames "$1" "$2" --); then
        echo "ci-scope: git diff $1 $2 failed; running every job" >&2
        echo false
        return 0
    fi
    printf '%s\n' "$list" | classify
}

self_test() {
    local self failures=0 total=0
    self=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
    check() {   # check <expected> <label> <command...>; stdin is the input
        local expected=$1 label=$2 got
        shift 2
        got=$("$@" 2>/dev/null)
        total=$((total + 1))
        if [[ $got == "$expected" ]]; then
            echo "ok   $label"
        else
            echo "FAIL $label: expected $expected, got '${got}'"
            failures=$((failures + 1))
        fi
    }
    list() { printf '%s\n' "$@" | bash "$self" classify; }

    check true  "one docs page"                 list docs/best-practices.md
    check true  "nested docs page"              list docs/playbooks/release-upgrade.md
    check true  "design page"                   list design/ci-strategy.md
    check true  "top-level Markdown files"      list README.md CHANGELOG.md ROADMAP.md ARCHITECTURE.md
    check true  "docs and design together"      list docs/doc-map.md design/decisions.md CONTRIBUTING.md
    check false "docs/reference.md (Tier 1 reads it)" list docs/reference.md
    check false "reference.md among docs pages" list docs/doc-map.md docs/reference.md
    check false "docs plus source"              list docs/best-practices.md src/pg_cuvs.c
    check false "docs plus expected output"     list README.md test/expected/edge_cases.out
    check false "docs plus SQL"                 list docs/best-practices.md sql/pg_cuvs--0.9.0.sql
    check false "docs plus Makefile"            list README.md Makefile
    check false "docs plus workflow"            list README.md .github/workflows/ci.yml
    check false "docs plus this script"         list README.md .github/scripts/ci-scope.sh
    check false "docs plus a script"            list README.md scripts/docs-contract-check.sh
    check false "non-Markdown file under docs"  list docs/data/stageA_exact_v3.csv
    check false "text file under design"        list design/specs/notes.txt
    check false "Markdown outside the scope"    list infra/cuvs-patches/README.md
    check false "Markdown under test"           list test/README.md
    check false "Markdown under bench"          list bench/protocol/README.md
    check false "quoted non-ASCII name"         list '"docs/\352\260\200.md"'
    check false "name with a space"             list 'docs/a b.md'
    check false "md suffix that is not .md"     list docs/notes.mdx
    check false "empty list"                    bash "$self" classify </dev/null
    check false "blank lines only"              list '' ''
    check true  "trailing line without newline" bash "$self" classify < <(printf 'README.md')

    # diff mode, on a throwaway repository.
    local repo
    repo=$(mktemp -d)
    (
        set -e
        export GIT_AUTHOR_NAME=fixture GIT_AUTHOR_EMAIL=fixture@example.invalid
        export GIT_COMMITTER_NAME=fixture GIT_COMMITTER_EMAIL=fixture@example.invalid
        export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
        cd "$repo"
        git init -q
        mkdir -p docs src
        echo a > docs/a.md; echo b > src/b.md; echo c > src/c.c
        git add . && git commit -qm base && git tag base
        echo more >> docs/a.md && git commit -qam docs && git tag docs
        git checkout -q base && git mv src/b.md docs/b.md && git commit -qm rename-in && git tag rename-in
        git checkout -q base && git mv docs/a.md docs/z.md && git commit -qm rename-docs && git tag rename-docs
        git checkout -q base && git mv docs/a.md src/a.md && git commit -qm rename-out && git tag rename-out
        git checkout -q base && echo d >> src/c.c && echo e >> docs/a.md && git commit -qam mixed && git tag mixed
    ) >/dev/null 2>&1 || { echo "FAIL could not build the fixture repository"; rm -rf "$repo"; return 1; }
    diff_in() { (cd "$repo" && bash "$self" diff "$@"); }
    check true  "diff: docs change"                     diff_in base docs
    check true  "diff: rename inside docs"              diff_in base rename-docs
    check false "diff: rename from src into docs"       diff_in base rename-in
    check false "diff: rename from docs into src"       diff_in base rename-out
    check false "diff: docs and code"                   diff_in base mixed
    check false "diff: no change"                       diff_in base base
    check false "diff: unknown base"                    diff_in no-such-rev docs
    check false "diff: outside a git repository"        bash -c 'cd / && bash "$0" diff HEAD^1 HEAD' "$self"
    rm -rf "$repo"

    echo "ci-scope self-test: $((total - failures))/$total passed"
    ((failures == 0))
}

case "${1:-}" in
    classify) classify ;;
    diff) [[ $# -eq 3 ]] || { echo "usage: $0 diff BASE HEAD" >&2; echo false; exit 0; }; diff_scope "$2" "$3" ;;
    self-test) self_test ;;
    *) echo "usage: $0 classify | diff BASE HEAD | self-test" >&2; exit 2 ;;
esac
