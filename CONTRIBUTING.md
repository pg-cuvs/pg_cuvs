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
