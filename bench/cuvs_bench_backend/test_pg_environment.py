"""GPU benchmark connections must not silently use libpq's default host."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pg_engine import PgEngine  # noqa: E402


@pytest.mark.parametrize("host", [None, "", "   "])
def test_gpu_environment_requires_explicit_host(monkeypatch, host):
    if host is None:
        monkeypatch.delenv("PGHOST", raising=False)
    else:
        monkeypatch.setenv("PGHOST", host)
    with pytest.raises(RuntimeError, match="explicit PGHOST"):
        PgEngine(require_gpu_environment=True)
