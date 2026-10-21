# Production RMM follow-up: blocked before CUDA validation

## Source and CPU results

GUC PR #192 was rebase-merged to `d851f5c`. RMM was rebased on that main;
normal conflicts in GUC registration, wire declarations and unit registration
were combined. Final application source pin: `0ff9f7c`.

- macOS unit: 289 + corpus27 passed; docs contract passed.
- Linux/aarch64 PG16.15/pgvector0.8.0: unit289 + corpus60 passed.
- Full CPU shim extension/production/fault daemon build/install passed.
- SQL cagra_search_params/rmm_pool/smoke: 3/3; isolation: 6/6 passed.
- The setup's redundant `createdb postgres` failed because initdb already created
  that database. The running fixture was reused, and subsequent SQL gates passed.
- Original 83f11cf implementation is preserved by the historical code patch and
  the earlier measured-sources bundle; original measurement raw bytes are unchanged.

## Failed transport attempt

One resume of the existing lane-owned L40S v6nc08sjs began at 00:41:48Z.
Brev SSH refresh initially lacked the starting VM alias. A second refresh restored
it, and hostname returned brev-v6nc08sjs. The runner had merged SSH warning stderr
with hostname stdout; strict string equality failed before copying any tools,
checking out source, compiling CUDA, deploying, checking graph hashes, or SQL work.
Both streams, traceback and the first runner are preserved in transport-r1.

The attempted cleanup helper was absent because copying had not started. An own
service cleanup connection later timed out. Normal brev stop was accepted at
00:43:10Z and official STOPPED was confirmed at 00:49:34Z. There was one resume
and one stop; no alternative VM, protected Crusoe connection, reset or delete.
The final status and conservative elapsed/cost calculations are in outcome.json.
The original runner's earlier 146-second observation ended while STOPPING and
must not be interpreted as a confirmed running-time/billing bound.

Readiness now keeps stdout and stderr separate while retaining exact hostname
identity equality. The revised scripts compile as Python and pass bash syntax
checking; they have not run on the VM. No second resume was admitted: the remaining
900-second total budget could not accommodate the observed stop latency and the
full build/two-arm measurement/cleanup plan with adequate time.

## Unmet gates

Production CUDA compile/co-deploy, actual binary mapping, GUC0/RMM0/default512x50
smoke, positive pool status and quota rejection: all unexecuted. New raw timing
CSV does not exist. Original graph/snapshot hashes were not freshly read on this
attempt; there was no source/DB/graph deployment command. Prior prototype evidence
is historical and does not satisfy these gates. RMM PR was not created.
