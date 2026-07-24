# infra/ — provisioning + operational scripts

Provisioning and the operational/verification scripts run against a provisioned GPU host.
Benchmark *harnesses* live under [`bench/`](../bench/), not here.

## Providers

pg_cuvs development has run on three GPU providers. **Brev is the main provider now.**

| Provider | Status | Where |
|----------|--------|-------|
| **Brev** (Massed Compute A100) | **main** | Bootstrap kept in the private docs repo (`pg_cuvs_docs/vm-access/brev-bootstrap/`) — it is a zero-to-ready `bootstrap.sh` (apt + conda + libcuvs + PG16 + build). Not committed here because it is operational access tooling, not product infra. |
| **GCP** (A100 via Terraform) | available | [`gcp/`](gcp/) — `terraform apply` provisions the instance and runs `scripts/install_gpu_env.sh` as the startup script. |
| **RunPod** | historical | Used earlier; no committed provisioning (pods were created ad hoc via `runpodctl`). Superseded by Brev. |

Brev cannot `stop` and has no persistent volume, so "restart" means a fast rebuild from
the bootstrap — see the private repo's `brev-bootstrap/README.md`. Operational lifecycle
notes (and how the GCP stop/start model differs) are in
[`docs/playbooks/gpu-vm-lifecycle.md`](../docs/playbooks/gpu-vm-lifecycle.md).

## Layout

```
infra/
  gcp/                    Terraform for a GCP A100 dev VM (main.tf, variables, outputs,
                          scripts/install_gpu_env.sh startup script)
  scripts/
    setup/                one-time host setup (postinstall, vram-budget-default)
    tests/                fault-injection / e2e / durability (integration-test, e2e-smoke,
                          leak-verify, delta-restart-e2e, objstore-roundtrip-e2e,
                          max-indexes-scale)
    benchmark/            large-dataset benchmark drivers (benchmark, benchmark-multigpu)
    recipes/              reusable SQL (tenant/multigpu partition recipes, pgbench scripts)
```

Most `scripts/` are invoked from the `Makefile` `gpu-*` targets (`make gpu-test`,
`gpu-smoke`, `gpu-bench`, …), piped over SSH to `$(VM_HOST)`.
