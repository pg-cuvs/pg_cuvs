"""Check that changes to the frontier or boot fence break the overlay fixture."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
MUTATIONS = {
    "accept a coverage gap": (
        "if (request->from != overlay->covered_upto)",
        "if (request->from < overlay->covered_upto)",
    ),
    "retain a prior boot": (
        "if (boot_id == overlay->boot_id)",
        "if (overlay->boot_id != 0)",
    ),
    "reuse the boot generation": (
        ".generation = overlay->generation + 1",
        ".generation = overlay->generation",
    ),
    "drop the search's prefix reference": (
        "__atomic_fetch_add(&chunk->refs, 1, __ATOMIC_RELAXED);",
        "(void) chunk;",
    ),
}

with tempfile.TemporaryDirectory(prefix="pgcuvs-overlay-test-") as path:
    directory = Path(path)
    source = directory / "cuvs_page_overlay.c"
    target = directory / "policy"
    original = (SRC / source.name).read_text()
    cc = shlex.split(os.environ.get("CC", "cc"))
    for name in ("baseline", *MUTATIONS):
        modified = original
        if name in MUTATIONS:
            before, after = MUTATIONS[name]
            assert original.count(before) == 1, (name, "wrong mutation anchor")
            modified = original.replace(before, after)
        source.write_text(modified)
        subprocess.run([*cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(SRC),
                        str(ROOT / "test/unit/test_page_overlay.c"), str(source), "-lm",
                        "-o", str(target)], check=True)
        result = subprocess.run([str(target)], cwd=directory, capture_output=True, text=True)
        if name == "baseline":
            assert result.returncode == 0, result.stderr
            print(result.stdout, end="")
        else:
            assert result.returncode != 0 and "Assertion" in result.stderr, name
            print(f"RED {name}")
