# Tier 2 GPU CI — one-time setup

The Tier 2 workflow (`.github/workflows/gpu.yml`) runs the real A100 regression
**on demand** from the Actions UI. It always needs a self-hosted runner on the
GPU VM. Automatic VM start/stop additionally needs keyless GCP auth (WIF) and
GitHub Actions variables. Without `GCP_WIF_PROVIDER`, the workflow skips VM
start/stop and requires an already-running VM with an online runner.

No long-lived secret is stored in GitHub: GCP trusts short-lived GitHub OIDC
tokens scoped to **this repo only** when WIF is configured. A maintainer starts
the workflow from the Actions UI; an already-running VM can keep its runner
online outside that workflow.

Concrete values below assume: repo `pg-cuvs/pg_cuvs`, project
`your-gcp-project`, instance `pg-cuvs-dev`, zone `us-central1-b`. Adjust.

---

## 1. Workload Identity Federation (keyless GCP auth) — run with project-admin gcloud

```bash
PROJECT=your-gcp-project
PROJECT_NUM=$(gcloud projects describe "$PROJECT" --format='value(projectNumber)')
REPO=pg-cuvs/pg_cuvs

# Pool + provider that trusts GitHub's OIDC issuer, RESTRICTED to this repo.
gcloud iam workload-identity-pools create gh-pool \
  --project="$PROJECT" --location=global --display-name="GitHub Actions"

gcloud iam workload-identity-pools providers create-oidc gh-provider \
  --project="$PROJECT" --location=global --workload-identity-pool=gh-pool \
  --display-name="GitHub OIDC" \
  --issuer-uri="https://token.actions.githubusercontent.com" \
  --attribute-mapping="google.subject=assertion.sub,attribute.repository=assertion.repository" \
  --attribute-condition="assertion.repository=='${REPO}'"   # ← the security boundary

# Least-privilege service account: start/stop/get this one instance only.
gcloud iam service-accounts create gpu-ci --project="$PROJECT" --display-name="GPU CI"
SA=gpu-ci@${PROJECT}.iam.gserviceaccount.com
gcloud compute instances add-iam-policy-binding pg-cuvs-dev --zone=us-central1-b \
  --project="$PROJECT" --member="serviceAccount:${SA}" --role="roles/compute.instanceAdmin.v1"
# (instanceAdmin.v1 on the single instance covers start/stop/get. Tighter: a
#  custom role with compute.instances.{start,stop,get} only.)

# Let ONLY this repo's workflows impersonate the SA.
gcloud iam service-accounts add-iam-policy-binding "$SA" --project="$PROJECT" \
  --role="roles/iam.workloadIdentityUser" \
  --member="principalSet://iam.googleapis.com/projects/${PROJECT_NUM}/locations/global/workloadIdentityPools/gh-pool/attribute.repository/${REPO}"

# Print the two non-secret identifiers to paste into GitHub (step 3):
echo "GCP_WIF_PROVIDER = projects/${PROJECT_NUM}/locations/global/workloadIdentityPools/gh-pool/providers/gh-provider"
echo "GCP_CI_SA        = ${SA}"
```

> Tighten further if desired: add `attribute.environment` / `attribute.ref`
> conditions so only the `gpu` environment (step 3) or `main` can mint a token.

## 2. Self-hosted runner on the VM (runs the real build as the build user)

On the VM (as `ubuntu`), register a runner with labels `gpu,a100` and install it
as a **boot-start systemd service** so it comes online when `gpu.yml` starts the VM:

```bash
# On the VM:
mkdir -p ~/actions-runner && cd ~/actions-runner
curl -o runner.tar.gz -L https://github.com/actions/runner/releases/latest/download/actions-runner-linux-x64.tar.gz
tar xzf runner.tar.gz
# Get a registration token: GitHub repo → Settings → Actions → Runners → New self-hosted runner.
./config.sh --url https://github.com/pg-cuvs/pg_cuvs --token <RUNNER_TOKEN> \
  --labels gpu,a100 --name pg-cuvs-a100 --unattended
sudo ./svc.sh install ubuntu     # run as ubuntu (has the cuvs_dev conda env)
sudo ./svc.sh start
sudo systemctl enable actions.runner.* 2>/dev/null || true   # start on boot
```

The runner is online while the VM is started;
`gpu.yml`'s `gpu-test` job queues until it appears, then runs `make installcheck`
and `make installcheck-isolation` as `ubuntu`.

## 3. GitHub Actions variables + environment

Repo → Settings → Secrets and variables → **Actions → Variables** (these are
identifiers, not secrets):

| Variable | Value |
|----------|-------|
| `GCP_WIF_PROVIDER` | the provider resource name printed in step 1 |
| `GCP_CI_SA` | `gpu-ci@your-gcp-project.iam.gserviceaccount.com` |
| `GPU_CI_INSTANCE` | `pg-cuvs-dev` |
| `GPU_CI_ZONE` | `us-central1-b` |

Repo → Settings → **Environments → New environment `gpu`** → add **Required
reviewers** (yourself). With WIF configured, this gates the `start-vm` job before
it starts the VM. It does not gate an already-running runner in the no-WIF path
or stop charges for a VM that is already on.

---

## Use it

Actions tab → **GPU regression (Tier 2)** → **Run workflow** → pick a branch (or
enter a PR number) → Run. Only users with write access see the button. The VM
boots, the real installcheck and isolation suite run on the A100, and the VM
is stopped by the `stop-vm` job, including when `gpu-test` fails. With WIF unset,
start and stop the VM separately; the workflow does not manage its lifetime.
