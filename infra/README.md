# infra/ — provisioning + operational scripts

Provisioning and the operational/verification scripts run against a provisioned GPU host.
Benchmark *harnesses* live under [`bench/`](../bench/), not here.

## Providers

pg_cuvs development has run on three GPU providers. **Brev is the main provider now.**

| Provider | Status | Where |
|----------|--------|-------|
| **Brev** (Massed Compute A100) | **main** | [`brev/`](brev/) — `bootstrap.sh` takes a fresh instance to ready (apt + conda + libcuvs + PG16 + build) in ~10 min. Same category as the GCP startup script below; access details (IPs, project ids) stay in the private docs repo, the script does not. |
| **GCP** (A100 via Terraform) | available | [`gcp/`](gcp/) — `terraform apply` provisions the instance and runs `scripts/install_gpu_env.sh` as the startup script. |
| **RunPod** | historical | Used earlier; no committed provisioning (pods were created ad hoc via `runpodctl`). Superseded by Brev. |

Brev cannot `stop` and has no persistent volume, so "restart" means a fast rebuild from
the bootstrap — see [`brev/README.md`](brev/README.md). Operational lifecycle
notes (and how the GCP stop/start model differs) are in
[`docs/playbooks/gpu-vm-lifecycle.md`](../docs/playbooks/gpu-vm-lifecycle.md).

## Layout

```
infra/
  brev/                   Brev/Massed Compute A100 (current main): zero-to-ready
                          bootstrap.sh + README with the restart procedure and the
                          build gotchas it encodes
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
