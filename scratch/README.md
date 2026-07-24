# scratch/ — staging space for in-development scripts

Write temporary, in-development scripts here. **Move them to a proper home once they
settle** — this directory is a staging area, not a permanent one.

This was `tools/` until #94. An audit found that every file in it had actually belonged to
a `bench/` subtree all along (protocol spikes, a Pareto post-processor, a filter-sweep
benchmark), and the drift went unnoticed because "tools" implied permanent utilities. It
was renamed to `scratch/` so the name states the intent — transient, will be moved. Keeping
this note here prevents the drift from recurring: a script that outlives its scratch phase
gets a real home instead of accreting.

## Where things graduate to

| A script that is… | belongs in |
|---|---|
| a benchmark harness or its helper | `bench/` (pick the generation dir: `cuvs_bench_backend/`, `filter_recall/`, or `legacy/`) |
| a competitor/primitive spike a protocol engine drives | `bench/protocol/spikes/` |
| protocol result post-processing | `bench/protocol/` |
| a one-off that produced a committed result or doc | move it next to what it produced, and note it in that dir's README |
| genuinely throwaway | delete it when done, don't commit it |

If a script here starts being referenced from a doc, a Makefile, or another script, that
is the signal it has outgrown `scratch/` — relocate it and rewrite the references in the
same commit.
