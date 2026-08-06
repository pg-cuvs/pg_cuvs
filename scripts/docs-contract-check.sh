#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

errors=0

fail() {
    printf '[FAIL] %s\n' "$1" >&2
    errors=$((errors + 1))
}

require_file() {
    local file="$1"
    if [[ ! -f "$file" ]]; then
        fail "missing file: $file"
    fi
}

require_text() {
    local file="$1"
    local needle="$2"
    if ! grep -Fq -- "$needle" "$file"; then
        fail "$file does not contain: $needle"
    fi
}

reject_text() {
    local file="$1"
    local needle="$2"
    if grep -Fq -- "$needle" "$file"; then
        fail "$file contains stale contract text: $needle"
    fi
}

control_version="$(sed -n "s/^default_version = '\([^']*\)'.*/\1/p" pg_cuvs.control | head -n 1)"
make_version="$(sed -n 's/^EXTVERSION[[:space:]]*=[[:space:]]*//p' Makefile | head -n 1)"
header_version="$(sed -n 's/^#define PG_CUVS_VERSION "\([^"]*\)".*/\1/p' src/cuvs_version.h | head -n 1)"

if [[ -z "$control_version" || -z "$make_version" || -z "$header_version" ]]; then
    fail "could not read all extension version declarations"
else
    [[ "$control_version" == "0.7.0" ]] || fail "pg_cuvs.control version is $control_version, expected 0.7.0"
    [[ "$make_version" == "$control_version" ]] || fail "Makefile EXTVERSION ($make_version) differs from control ($control_version)"
    [[ "$header_version" == "$control_version" ]] || fail "PG_CUVS_VERSION ($header_version) differs from control ($control_version)"
fi

require_file "sql/pg_cuvs--0.4.0--0.5.0.sql"
require_file "test/sql/upgrade_path.sql"

require_text "docs/reference.md" '| `cuvs.filter_auto_threshold` | real | `0.0` |'
require_text "docs/reference.md" '| `cuvs.stream_bf_selectivity_threshold` | real | `0.004` |'
require_text "docs/reference.md" 'not a portable crossover'
require_text "ROADMAP.md" 'Current contract (2026-08-04)'
require_text "ROADMAP.md" '#124'
require_text "ROADMAP.md" '**BLOCKER**'

require_text "docs/experiments/benchmark-archive.md" 'Historical multi-tenant filtered search sweep — pre-#80'
require_text "BENCHMARK.md" 'Current routing contract (source/reference verified)'
require_text "BENCHMARK.md" '`cuvs.filter_auto_threshold=0.0`'
require_text "BENCHMARK.md" '`cuvs.stream_bf_selectivity_threshold=0.004`'
require_text "docs/experiments/filter-threshold-experiment.md" 'Status: Historical experiment'
require_text "README.md" 'Known artifact caveat'
require_text "README.md" '`index_bytes=0`'
require_text "README.md" 'bench/results/README.md'
require_text "bench/results/README.md" '**known defect**'
require_text "design/ops-gpu-playbook.md" '| 0 또는 생략 (기본) | GPU별 물리 VRAM의 90%를 budget으로 사용 |'
reject_text "design/ops-gpu-playbook.md" '40000 (기본)'
require_text "docs/playbooks/rollback-and-cleanup.md" ' 0.7.0'

reject_text "docs/playbooks/README.md" '0.1.0 설치/재설치'
reject_text "docs/playbooks/release-upgrade.md" '현재 default_version은 **0.1.0**'
reject_text "docs/playbooks/release-upgrade.md" '기대: 0.1.0'
reject_text "docs/playbooks/release-upgrade.md" '첫 릴리스 후 cross-version upgrade 검증 필요'

if (( errors > 0 )); then
    printf '[FAIL] documentation contract checks: %d failure(s)\n' "$errors" >&2
    exit 1
fi

printf '[OK] documentation contract checks passed\n'
