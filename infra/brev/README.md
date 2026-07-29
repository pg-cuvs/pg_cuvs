# Brev pg_cuvs GPU VM — fast rebuild

**There is no "resume".** A Brev A100 does not support `brev stop`, has no
persistent volume, and Brev has no snapshot/image feature. `brev delete` is the only way to stop paying, and it loses everything.
So the plan is not to preserve the VM — it is to rebuild it fast.

`bootstrap.sh` does that: a fresh instance → ready (built extension + daemon +
PG16 preloaded + wiki_all_1M dataset) in ~10 minutes, with every gotcha from the
2026-07-23 build session baked in so none of it has to be re-debugged.

## Restart procedure

```bash
brev start <name>            # or create a new massedcompute A100 in the console
brev refresh                 # writes ~/.brev/ssh_config so `ssh <name>` works
scp infra/brev/bootstrap.sh <name>:/home/shadeform/
ssh <name> 'bash /home/shadeform/bootstrap.sh 2>&1 | tee bootstrap.log'
```

Wall clock: setup ~80s, dataset ~75s, build ~2–3 min, verify ~30s.

Which instance type: benchmarking is pinned to `massedcompute_A100_sxm4_80G`
because the canonical numbers came from it; development and regression work only
needs an A100-class GPU. See the `gpu-vm-provision` skill for the rule and the
verified alternatives.

## The daemon

`bootstrap.sh` installs a `pg-cuvs-server` systemd unit, so the daemon outlives
the ssh session and the `systemctl`/`journalctl` commands used throughout the
playbooks actually work:

```bash
sudo systemctl restart pg-cuvs-server
systemctl is-active pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

The socket appears only after the daemon's first CUDA context — 12s on one A100,
over 3 minutes on another — so the unit carries `TimeoutStartSec=600` and sitting
in `activating` for minutes is normal, not a hang. `ExecStartPost` polls for the
socket and widens it to 0666; the daemon creates it 0660, which the `postgres` OS
user cannot open, and doing that chmod by hand is the step that gets forgotten.

Indexes live in `/tmp/cuvs_indexes`, matching what the 35 regression SQL files
hardcode — the suite passes straight off a fresh bootstrap with no manual daemon
restart. `/tmp` being volatile is fine here since nothing on the VM is a source
of truth; on a long-lived host, watch for `systemd-tmpfiles` reaping it.

## What survives a delete (and what doesn't)

| Asset | Survives delete? | Recovery |
|---|---|---|
| Source code | yes — it's on `main` | `git clone` (bootstrap step 3) |
| Build env, dataset, daemon | **no** | bootstrap rebuilds from scratch |
| Measurement CSVs | yes — committed under `bench/results/` | in the repo |

Nothing on the VM is a source of truth. If you delete it, the only cost is the
~10 min rebuild, not lost work.

## Gotchas the script encodes (so you don't re-hit them)

These cost the most time on 2026-07-23; each is now a one-liner in `bootstrap.sh`:

1. **`ld: cannot find -lstdc++`** — image has gcc=12 but g++=11, so the static
   `libstdc++.a` the extension links only existed for gcc-11. Fix: install
   `libstdc++-12-dev`.
2. **`nvcc fatal: cannot execute cc1plus`** — without `conda activate`, conda's
   nvcc can't find its own cc1plus. Fix: `make NVCC="nvcc -ccbin /usr/bin/g++"`.
3. **PG won't restart after editing `environment`** — Debian's
   `/etc/postgresql/16/main/environment` requires the value **quoted**:
   `LD_LIBRARY_PATH='...'`.
4. **`unrecognized configuration parameter "cuvs.*"`** — the custom GUCs don't
   exist until the extension is preloaded. Set `shared_preload_libraries` and
   restart FIRST, then the `cuvs.*` GUCs.
5. **`connection to /tmp/.s.PGSQL.5432 failed`** — conda's libpq defaults its
   unix socket to `/tmp`, Debian PG uses `/var/run/postgresql`. Bench needs
   `export PGHOST=/var/run/postgresql`.
6. **`ModuleNotFoundError: psycopg` / `pgvector`** — the bench backend imports
   `psycopg` (v3) and `pgvector.psycopg`, not just psycopg2. Both are installed
   into the separate `cuvs_bench` env (kept apart because cuvs-bench pulls
   python 3.12 and would break the build env).
7. **conda under a 0700 home blocks the `postgres` OS user from reading
   libcuvs** — put miniforge in `/opt` and `chmod -R o+rX`. Never add conda's
   lib dir to system ldconfig (breaks sshd via OpenSSL mismatch); symlink just
   `libstdc++.so.6`/`libgcc_s.so.1` into `/usr/local/lib` instead.

## If the provider/user differs

The script targets a shadeform-family image (`shadeform` user, systemd) — both
massedcompute and paperspace qualify. On a container without systemd (e.g.
RunPod, user `root`): change `USER_HOME`, swap `systemctl restart
postgresql@16-main` for `pg_ctlcluster 16 main restart`, and replace the
`pg-cuvs-server` unit with a supervised launch of your own — the daemon is a unit
now, so there is no nohup fallback left in the script to fall back to.
