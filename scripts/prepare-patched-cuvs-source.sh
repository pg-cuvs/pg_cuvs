#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
# Recreate the pinned cuVS source plus infra/cuvs-patches 0001..0006 for CPU
# source tests. No build. Usage: prepare-patched-cuvs-source.sh UPSTREAM_GIT OUT_DIR
set -euo pipefail
readonly COMMIT="60b19a0f62ba0bd716de349382828e43be6e2daa"
[[ $# -eq 2 ]] || { echo "usage: $0 UPSTREAM_GIT OUT_DIR" >&2; exit 2; }
readonly UPSTREAM="$1" OUT="$2"
readonly PATCHES="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../infra/cuvs-patches" && pwd)"
[[ ! -e "$OUT" ]] || { echo "output must not exist: $OUT" >&2; exit 2; }
mkdir -p "$OUT"
GIT_NO_LAZY_FETCH=1 git -C "$UPSTREAM" archive "$COMMIT" cpp LICENSE | tar -x -C "$OUT"
git -C "$OUT" init -q
git -C "$OUT" add -A
git -C "$OUT" -c user.name=prepare -c user.email=prepare@localhost commit -qm "cuvs $COMMIT"
for patch in "$PATCHES"/000[1-6]-*.patch; do
  git -C "$OUT" apply --check "$patch"
  git -C "$OUT" apply "$patch"
  git -C "$OUT" add -A
  git -C "$OUT" -c user.name=prepare -c user.email=prepare@localhost commit -qm "$(basename "$patch")"
  git -C "$OUT" tag "level-$(basename "$patch" | cut -c1-4)"
done
python3 -I - "$PATCHES/patchset-manifest.json" "$PATCHES" "$OUT" <<'PY'
import hashlib, json, pathlib, sys
manifest, patches, out = (pathlib.Path(x) for x in sys.argv[1:])
m = json.loads(manifest.read_text())
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert [e["path"] for e in m["patches"]] == sorted(p.name for p in patches.glob("000[1-6]-*.patch"))
for e in m["patches"]:
    assert sha(patches / e["path"]) == e["sha256"], e["path"]
for path, e in m["source_files"].items():
    assert sha(out / path) == e["final_sha256"], path
mk = m["runtime_markers"]["resident_nn_upper"]
digest = hashlib.sha256(b"".join((out / f).read_bytes() for f in mk["content_digest"]["files"])).hexdigest()
assert digest == mk["content_digest"]["value"] and mk["value"].endswith("@" + digest)
assert ('"' + mk["value"] + '"') in (out / "cpp/src/neighbors/hnsw.cpp").read_text()
print(f"prepared {out}: 0001..0006 applied, {len(m['source_files'])} manifest files match")
PY
