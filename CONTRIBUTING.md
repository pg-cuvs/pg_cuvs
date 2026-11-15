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
- Tests: `make installcheck` and `make installcheck-isolation` on a configured GPU
  host. The CPU-reference CI path builds with `PGCUVS_CPU_SHIM=1`, starts a shim
  daemon, then runs `make installcheck-tier1` and `make installcheck-isolation`;
  `make test-unit` runs separately. See [ci.yml](.github/workflows/ci.yml).
- Before opening a pull request, go through the list in the pull request template
  ([.github/pull_request_template.md](.github/pull_request_template.md)) and tick what
  applies in the description. The list is a matter of judgement, and nothing enforces
  it; the two CI jobs are the mechanical checks.
  - Run `make docs-contract-check` and `make test-unit`.
  - If the change touches the Makefile's GPU compile or link flags, `infra/cuvs-patches`
    or the SDK planner sources, run `scripts/check-sdk-patch-path.sh`. It applies
    `infra/cuvs-patches` to the pinned cuVS source, checks the manifest hashes and the
    planner source, and checks the Makefile flags for a build with the patched SDK
    planner. It needs git, python3 and make, not CUDA or `pg_config`, and fetches the
    pinned cuVS source once into `~/.cache/pg_cuvs`. Neither CI job builds that path;
    the Unit tests (CPU) job runs this check. In CI it fetches the pinned cuVS source
    from github.com on every run, so a failed fetch turns that job red; there is no
    retry. It checks flags, not compilation.
  - If the change touches a GPU path, say in the description what was run on a GPU,
    on which hardware and toolchain, and what was not run. CI has no GPU.
  - List what was not verified. A reader should not have to guess it from what is absent.
  - Add a `CHANGELOG.md` entry under `[Unreleased]` for a user-visible change.
- Follow the coding guidelines in [CLAUDE.md](CLAUDE.md): surgical changes,
  tests-first where practical, and structural vs behavioral changes in separate commits.

## Branches and releases

### Branches

| Branch | Role | How it changes |
|---|---|---|
| `main` | Integration branch. Carries the version in development. | Pull request only; both CI jobs (`Unit tests (CPU)`, `Tier 1 — CPU shim (installcheck)`) must pass. Squash merge only; the head branch is deleted after the merge. No force-push, no deletion. |
| `release/vX.Y.Z` | Stabilization branch of one release, cut from `main`. | Pull request only, same checks. Receives fixes that are already on `main` (below), no features and no SQL script changes (a hotfix branch adds the scripts of its own version, see Hotfixes). Kept after the tag. |
| `vX.Y.Z` (tag) | The released commit. | Annotated tag on the last commit of `release/vX.Y.Z`, pushed after the last pull request into that branch has merged. Moved or deleted only while its release is unpublished (checklist step 8), never afterwards. |
| topic branches | One change each. | Branch from `main`; delete after the merge. |

`.github/workflows/ci.yml` runs on pull requests into `main` and into `release/**`
and on pushes to both.

The repository deletes the head branch of a merged pull request, so a release branch
is never the head of one. Before merging a pull request whose branch is the base of
another open pull request, list those (`gh pr list --base <branch>`): GitHub closes a
pull request whose base branch disappears, and a closed one can be neither retargeted
nor reopened.

A change goes to `main` first. If it must also be in a release that is already cut,
cherry-pick the squash commit from `main` onto a topic branch based on the release
branch (`git cherry-pick -x <commit>`) and open a pull request into
`release/vX.Y.Z`. A fix that only exists on the release branch is the exception;
say why in the pull request.

Squash merges give the release branch commits that are not ancestors of `main`.
Tags on a release branch that carries cherry-picks are therefore not reachable
from `main`, and `git describe` on `main` does not see them. The extension version
comes from `pg_cuvs.control`, never from `git describe`.

### Versioning

pg_cuvs is 0.x. A version number is released once, and the version in development
is `default_version` in `pg_cuvs.control` on `main`.

- **patch** (`0.9.1` to `0.9.2`) is the default for everything: bug fixes, features,
  a new SQL function, view column or GUC, a C-only change. Nothing about a change
  makes it a minor bump just because it adds SQL.
- **minor** (`0.9.x` to `0.10.0`) only when an installed database or its data needs
  more than `ALTER EXTENSION pg_cuvs UPDATE` and a restart: an index or artifact
  format that old data cannot be read in (REINDEX required), the removal or rename
  of an SQL object or GUC, or a changed default that changes the results or routing
  of existing queries.
- **major** `1.0.0` is the first release that declares the SQL surface stable; after
  it, a breaking change to that surface is a major bump.

Every release has a new version number, also when no SQL object changed: the
library and the daemon changed, and the version is stamped into object-store
manifests (`PG_CUVS_VERSION`). In that case the upgrade script
`sql/pg_cuvs--<from>--<to>.sql` contains only the `\echo ... \quit` guard and
comments.

Why the number matters: `pg_extension.extversion` is per-database state, so every
version is an upgrade path that must keep working forever, and one more chance for
a database to sit on an old version while the `.so` has moved on. That drift is a
real ABI mismatch (a `.so` returning 40 `OUT` parameters against a 39-column SQL
declaration), and `infra/scripts/gpu-preflight.sh` check 3 exists to catch it.
Fewer versions, fewer ways to be wrong.

**SQL scripts of a version are never edited in place.** `sql/pg_cuvs--X.Y.Z.sql` and
`sql/pg_cuvs--<from>--X.Y.Z.sql` may change only while `X.Y.Z` is the version in
development on `main`. The commit that moves `default_version` past `X.Y.Z` freezes
them, because after that point databases installed from that version exist, and a
catalog that a later edit would change can no longer be repaired by an upgrade.
0.9.0 had no such point: seven commits changed its scripts after the commit that
named the version. A fix to a frozen script goes into the scripts of the version in
development. While a version is in development its scripts can still change, so a
database that has to keep working is installed from a release tag, not from `main`. `scripts/check-sql-frozen.sh` enforces this on every pull request
(`ci.yml`); on a pull request into `release/vX.Y.Z` it refuses every change to
`sql/pg_cuvs--*.sql` except adding the scripts of `X.Y.Z` itself. A last edit of the
old scripts must be merged before the bump commit, not with it.

Both scripts of the version in development change together: a change to the install
script `pg_cuvs--X.Y.Z.sql` has the matching statement in the upgrade script
`pg_cuvs--<from>--X.Y.Z.sql`, and `test/sql/upgrade_path.sql` compares the two
catalogs.

**Raising the version** (the pull request that follows every release cut) changes:

1. `pg_cuvs.control`: `default_version`.
2. `Makefile`: `EXTVERSION` and the two new scripts in `DATA`.
3. `src/cuvs_version.h`: `PG_CUVS_VERSION`.
4. `sql/pg_cuvs--<new>.sql`: a copy of the previous install script with its first
   line renamed; `sql/pg_cuvs--<old>--<new>.sql`: `\echo` guard and a comment.
5. `README.md` ("Current extension version" and the `extversion` example),
   `ROADMAP.md` (contract line), `docs/doc-map.md` (install script),
   `docs/playbooks/release-upgrade.md` and `docs/playbooks/rollback-and-cleanup.md`.

`make docs-contract-check` reads the version from `pg_cuvs.control` and reports
every one of these that disagrees. Then `git grep -n '<old version>'` for prose that
states the current version; mentions of where a feature was introduced and of old
upgrade paths stay.

### Release checklist

Run it in a clean checkout (a spare `git worktree` is enough), signed in with `gh auth
login` as a maintainer. `VER` is the version being released, which is the
`default_version` of `main`, and `PREV` is the previous release tag (`none` for the
first release with a changelog: its section starts after the baseline commit named
at the top of `CHANGELOG.md`).

```sh
VER=0.9.1; TAG=v$VER
git fetch origin --tags --prune
```

1. **`main` is ready.** The last CI run on `main` is green and `main` holds
   everything the release is to contain; every item of `[Unreleased]` in
   `CHANGELOG.md` is something that is in `main`, and every change since the previous
   release is in it.

   ```sh
   gh run list --repo pg-cuvs/pg_cuvs --branch main --workflow CI --limit 3
   git log --oneline "${PREV:-2afe8b2b}"..origin/main
   grep default_version pg_cuvs.control    # must say VER
   ```

2. **Changelog pull request into `main`.** In `CHANGELOG.md` rename `## [Unreleased]`
   to `## [$VER] - YYYY-MM-DD`, put one sentence of summary directly under that
   heading (it becomes the release title) and add an empty `## [Unreleased]` above
   it. Every measured number keeps its conditions: hardware, data size and type,
   parameters, number of runs and the statistic.

   ```sh
   git switch -c release-changelog-$VER origin/main
   $EDITOR CHANGELOG.md
   bash scripts/release-notes.sh $VER title       # must print "v$VER — <summary>"
   git commit -am "docs: changelog for $TAG"
   git push -u origin HEAD
   gh pr create --base main --title "docs: changelog for $TAG" --body "Release changelog for $TAG."
   gh pr checks --watch
   gh pr merge --squash
   ```

3. **Cut the branch** at that merge commit, which is the tip of `main` if nothing was
   merged after it; otherwise use the SHA that `gh pr view` prints.

   ```sh
   git fetch origin
   CUT=$(gh pr view <changelog PR number> --json mergeCommit --jq .mergeCommit.oid)
   git log -1 --format='%H %s' "$CUT"                     # the changelog commit
   git push origin "${CUT}:refs/heads/release/${TAG}"
   git fetch origin && test "$(git rev-parse origin/release/$TAG)" = "$CUT" && echo cut-ok
   ```

4. **Raise the version on `main`** to the next patch in one pull request, and merge it
   before any other change (the list under Versioning, `make docs-contract-check`
   must pass). This is what freezes the SQL scripts of `$VER`.

   ```sh
   NEXT=0.9.2
   git switch -c version-$NEXT-dev origin/main
   # edit the files of the list under Versioning to $NEXT, then
   make docs-contract-check
   git add -A && git commit -m "chore: bump the extension to $NEXT (in development)"
   git push -u origin HEAD
   gh pr create --base main --title "chore: bump the extension to $NEXT (in development)" --body "Raises the version in development after the cut of $TAG."
   gh pr checks --watch && gh pr merge --squash
   ```

5. **Check the release branch.** CI runs on every push to `release/**`. Wait for it,
   and run the release check that the tag workflow will run:

   ```sh
   gh run list --repo pg-cuvs/pg_cuvs --branch release/$TAG --workflow CI --limit 1
   gh run watch <run id> --exit-status
   git switch --detach origin/release/$TAG
   bash scripts/release-check.sh $TAG
   ```

   Run the GPU workflow on the branch only if a maintainer approves its cost (it starts
   a GPU VM): `gh workflow run gpu.yml --ref release/$TAG`. A release whose GPU run is
   red or was not made says so in its notes.

6. **Fixes** reach the branch only through the cherry-pick pull requests described
   above (`git cherry-pick -x <commit on main>`, pull request into
   `release/$TAG`). Each adds its line to the `## [$VER]` section on the branch. After
   the tag, one pull request into `main` copies the final `## [$VER]` section into
   `main`'s `CHANGELOG.md` and removes the lines it took over from `## [Unreleased]`.
   Repeat step 5 after each.

7. **Tag** the last commit of the branch and push the tag. Do it after the last pull
   request into the branch has merged.

   ```sh
   git fetch origin
   git tag -a $TAG -m "pg_cuvs $VER" origin/release/$TAG
   git push origin $TAG
   ```

8. **The draft.** The `Release draft` workflow (`.github/workflows/release.yml`) checks
   the tag and creates a draft release. It refuses a tag that is not on
   `release/$TAG` or whose `pg_cuvs.control`, `Makefile`, `PG_CUVS_VERSION`,
   `sql/pg_cuvs--$VER.sql` or `CHANGELOG.md` section does not match. If it refuses,
   fix the cause on the branch and, because nothing is published yet, move the tag:
   `git push origin --delete $TAG && git tag -d $TAG`, then step 7 again.

   ```sh
   gh run list --repo pg-cuvs/pg_cuvs --workflow "Release draft" --limit 1
   gh run watch <run id> --exit-status
   gh release view $TAG --json isDraft,name,tagName,body --jq '{isDraft,name,tagName}'
   ```

9. **Publish.** A maintainer reads the draft (title, notes, every number with its
   conditions) in the web UI or with `gh release view $TAG`, and publishes it. The
   workflow never publishes.

   ```sh
   gh release edit $TAG --draft=false --latest
   ```

10. **Verify** the published release, then keep `release/$TAG`: it is the record of
    what was cherry-picked into the release.

    ```sh
    gh release view $TAG --json tagName,name,isDraft,isPrerelease,publishedAt,url
    git ls-remote --tags origin "refs/tags/$TAG*"          # the tag object and, with ^{}, its commit
    test "$(git rev-parse "${TAG}^{commit}")" = "$(git rev-parse origin/release/$TAG)" && echo tag-on-branch-tip
    git show "${TAG}:pg_cuvs.control" | grep default_version  # VER
    git show "${TAG}:CHANGELOG.md" > "${TMPDIR:-/tmp}/changelog-$VER.md"
    test "$(gh release view $TAG --json body --jq .body | tr -d '\r')" = "$(CHANGELOG=${TMPDIR:-/tmp}/changelog-$VER.md bash scripts/release-notes.sh $VER body)" && echo notes-match-changelog
    gh release list --repo pg-cuvs/pg_cuvs --limit 3
    ```

Do not tag before step 7, and do not tag a commit that is not on the release branch:
a tag placed ahead of the final merge points at a commit that is not the release.

### GitHub releases

- Title: `vX.Y.Z — <summary>`, the summary being the first prose line of the
  `## [X.Y.Z]` section.
- Notes: the `## [X.Y.Z]` section of `CHANGELOG.md`, unchanged. Anything else
  belongs in the changelog first.
- A measured number is written with its conditions: hardware, data set size and
  type, parameters, number of runs and whether it is a median or a single run.
  A number without them does not go into a release.
- The release is created as a draft by the workflow and published by a person.

### Hotfixes

Prefer releasing a fix from `main` as the next patch. When `main` holds changes that
must not ship yet, start from the tag of the release to fix: create
`release/vX.Y.(Z+1)` at `vX.Y.Z`, raise the version on it in the first pull request
(the list under Versioning, nothing else; the upgrade script is empty), cherry-pick
the fix from `main`, add the `## [X.Y.(Z+1)]` section to `CHANGELOG.md`, and follow
steps 5 to 10. A hotfix carries no SQL change. Check first that the number is free (`git tag -l`,
and `default_version` on `main`); if `main` already develops that number, raise the
version in development on `main` past it in a separate pull request first, so that
no number is released twice.
