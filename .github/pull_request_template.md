## Summary

<!-- What changes, and why. -->

## Verification

<!-- What was run, where (OS, toolchain, GPU if any), and the result. -->

## Not verified

<!-- What was not run or not checked. Write "nothing" only if that is true. -->

## Before opening

Tick what applies; leave the rest and say why if it is not obvious. See
[CONTRIBUTING.md](https://github.com/pg-cuvs/pg_cuvs/blob/main/CONTRIBUTING.md#development).

- [ ] `make docs-contract-check`, `make test-unit` and `make test-source-checks` pass locally
- [ ] `scripts/check-sdk-patch-path.sh` passes (changes to GPU compile or link flags in the Makefile, `infra/cuvs-patches`, or the SDK planner sources)
- [ ] GPU paths: the description says what was run on a GPU and what was not
- [ ] "Not verified" lists what was not run
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]` (user-visible changes)
