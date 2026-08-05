# Contributing to pg_cuvs

pg_cuvs is built by **Team pg-cuvs**, an independent open-source project.
Contributions and collaboration inquiries are welcome.

## Ways to contribute

- **Issues** — bug reports, feature requests, benchmark results, or questions.
- **Pull requests** — for non-trivial changes, skim the design docs first:
  [ROADMAP.md](ROADMAP.md) (what / in what order), [design/specs/phase-record.md](design/specs/phase-record.md)
  (specs), and [design/decisions.md](design/decisions.md) (ADRs — the "why").
- **Collaboration** — for research collaboration or larger involvement, reach out
  at <ysys143@gmail.com>.

## Development

- Requirements, build, and quickstart: see [README.md](README.md).
- Tests: `make installcheck` on a GPU host, or the CPU-reference suite
  (`make PGCUVS_CPU_SHIM=1`, no GPU) for the plumbing / contract / correctness tiers.
- Follow the coding guidelines in [CLAUDE.md](CLAUDE.md): surgical changes,
  tests-first where practical, and structural vs behavioral changes in separate commits.

## Extension versioning

`EXTVERSION` (Makefile) and `default_version` (`pg_cuvs.control`) must agree, and
every change to them needs a matching `sql/pg_cuvs--<from>--<to>.sql` migration.

Pick the smallest bump that carries the change:

| Change | Bump | Example |
|---|---|---|
| No SQL-level object changed (C code only) | **none** | a daemon fix, a planner cost tweak |
| Additive and contract-preserving | **patch** `0.7.1` | a new column on `pg_stat_gpu_search` |
| New user-visible surface or changed semantics | **minor** `0.8.0` | a new SQL function, a new GUC that changes routing |
| First public release | `1.0.0` | |

A migration script is required whenever a SQL object changes — including for a
patch bump. Needing a script is **not** by itself a reason to bump the minor:
`CREATE OR REPLACE FUNCTION` cannot change `OUT` parameters, so even a single new
stats column forces a DROP/CREATE, and that is still a patch.

Why the rule exists: `pg_extension.extversion` is per-database state, so every
version is an upgrade path that must keep working forever, and one more chance
for a database to sit on an old version while the `.so` has moved on. That drift
is a real ABI mismatch — a `.so` returning 40 `OUT` parameters against a 39-column
SQL declaration — and `infra/scripts/gpu-preflight.sh` check 3 exists to catch it.
Fewer versions, fewer ways to be wrong.
